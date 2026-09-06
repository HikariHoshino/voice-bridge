from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import secrets
import socket
import ssl
import tempfile
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, replace
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Callable

import win32crypt
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID
from zeroconf import ServiceBrowser, ServiceInfo, ServiceListener, Zeroconf


PROTOCOL_VERSION = 2
SERVICE_TYPE = "_voicebridge._tcp.local."
MAX_TEXT_BYTES = 100 * 1024
SOCKET_TIMEOUT = 8
DISCOVERY_GROUP = "224.0.0.167"
DISCOVERY_PORT = 53318
MAX_BEACON_BYTES = 2048


def canonical_payload(message: dict) -> bytes:
    output = bytearray()
    for key in sorted(key for key in message if key != "signature"):
        key_bytes = key.encode("utf-8")
        value = message[key]
        if isinstance(value, bool):
            value_text = "true" if value else "false"
        else:
            value_text = str(value)
        value_bytes = value_text.encode("utf-8")
        output.extend(str(len(key_bytes)).encode("ascii"))
        output.extend(b":")
        output.extend(key_bytes)
        output.extend(str(len(value_bytes)).encode("ascii"))
        output.extend(b":")
        output.extend(value_bytes)
    return bytes(output)


def pairing_code(first_fingerprint: str, second_fingerprint: str, nonce: str) -> str:
    material = ":".join(sorted((first_fingerprint, second_fingerprint))) + f":{nonce}"
    digest = hashlib.sha256(material.encode("utf-8")).digest()
    number = (digest[0] << 16) | (digest[1] << 8) | digest[2]
    return f"{number % 1_000_000:06d}"


def message_auth(secret: str, message_id: str, text: str) -> str:
    key = _padded_base64(secret)
    digest = hmac.new(key, (message_id + "\0" + text).encode("utf-8"), hashlib.sha256).digest()
    return base64.b64encode(digest).decode("ascii").rstrip("=")


def _padded_base64(value: str) -> bytes:
    return base64.b64decode(value + "=" * (-len(value) % 4))


@dataclass(frozen=True)
class PeerInfo:
    id: str
    name: str
    device_type: str
    host: str
    port: int
    fingerprint: str
    trusted: bool = False
    state: str = "unpaired"
    last_seen: float = 0.0
    last_heartbeat: float = 0.0


class PeerIdentity:
    def __init__(self, directory: Path):
        self.directory = directory
        self.directory.mkdir(parents=True, exist_ok=True)
        self.metadata_file = directory / "peer-identity.json"
        self.certificate_file = directory / "peer-certificate.pem"
        self.private_key_file = directory / "peer-private-key.dpapi"
        metadata = self._read_json(self.metadata_file)
        self.id = metadata.get("id") if isinstance(metadata.get("id"), str) else str(uuid.uuid4())
        self.name = metadata.get("name") if isinstance(metadata.get("name"), str) else socket.gethostname()
        if not self.certificate_file.exists() or not self.private_key_file.exists():
            self._generate()
        self._write_json(self.metadata_file, {"id": self.id, "name": self.name})
        self.certificate = x509.load_pem_x509_certificate(self.certificate_file.read_bytes())
        protected = self.private_key_file.read_bytes()
        private_pem = win32crypt.CryptUnprotectData(protected, None, None, None, 0)[1]
        self.private_key = serialization.load_pem_private_key(private_pem, password=None)

    @property
    def fingerprint(self) -> str:
        return self.certificate.fingerprint(hashes.SHA256()).hex()

    @property
    def encoded_certificate(self) -> str:
        der = self.certificate.public_bytes(serialization.Encoding.DER)
        return base64.b64encode(der).decode("ascii")

    def sign(self, payload: bytes) -> str:
        signature = self.private_key.sign(payload, ec.ECDSA(hashes.SHA256()))
        return base64.b64encode(signature).decode("ascii").rstrip("=")

    def ssl_context(self, server: bool) -> ssl.SSLContext:
        purpose = ssl.Purpose.CLIENT_AUTH if server else ssl.Purpose.SERVER_AUTH
        context = ssl.create_default_context(purpose)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        private_pem = self.private_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
        temporary_path = None
        try:
            with tempfile.NamedTemporaryFile(prefix="voicebridge-key-", suffix=".pem", delete=False) as temp:
                temp.write(private_pem)
                temporary_path = temp.name
            context.load_cert_chain(str(self.certificate_file), temporary_path)
        finally:
            if temporary_path:
                try:
                    os.unlink(temporary_path)
                except OSError:
                    pass
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        return context

    def _generate(self) -> None:
        key = ec.generate_private_key(ec.SECP256R1())
        now = datetime.now(timezone.utc)
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, f"VoiceBridge-{self.id}")])
        certificate = (
            x509.CertificateBuilder()
            .subject_name(subject)
            .issuer_name(subject)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - timedelta(days=1))
            .not_valid_after(now + timedelta(days=3650))
            .sign(key, hashes.SHA256())
        )
        self.certificate_file.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
        private_pem = key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
        protected = win32crypt.CryptProtectData(private_pem, "VoiceBridge peer key", None, None, None, 0)
        self.private_key_file.write_bytes(protected)

    @staticmethod
    def _read_json(path: Path) -> dict:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    @staticmethod
    def _write_json(path: Path, data: dict) -> None:
        temporary = path.with_suffix(".tmp")
        temporary.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
        temporary.replace(path)


class TrustStore:
    def __init__(self, path: Path):
        self.path = path
        self.lock = threading.RLock()

    def get(self, peer_id: str) -> dict | None:
        with self.lock:
            data = PeerIdentity._read_json(self.path)
            value = data.get(peer_id)
            if not isinstance(value, dict) or not isinstance(value.get("fingerprint"), str):
                return None
            protected = value.get("secret_dpapi")
            if isinstance(protected, str):
                try:
                    secret = win32crypt.CryptUnprotectData(
                        base64.b64decode(protected), None, None, None, 0
                    )[1].decode("utf-8")
                except (OSError, ValueError, UnicodeDecodeError):
                    return None
                return {"fingerprint": value["fingerprint"], "secret": secret}
            secret = value.get("secret")
            if isinstance(secret, str):
                self.put(peer_id, value["fingerprint"], secret)
                return {"fingerprint": value["fingerprint"], "secret": secret}
            return None

    def put(self, peer_id: str, fingerprint: str, secret: str) -> None:
        with self.lock:
            data = PeerIdentity._read_json(self.path)
            protected = win32crypt.CryptProtectData(
                secret.encode("utf-8"), "VoiceBridge peer secret", None, None, None, 0
            )
            data[peer_id] = {
                "fingerprint": fingerprint,
                "secret_dpapi": base64.b64encode(protected).decode("ascii"),
            }
            PeerIdentity._write_json(self.path, data)


class _DiscoveryListener(ServiceListener):
    def __init__(self, owner: "DesktopPeerNode"):
        self.owner = owner

    def add_service(self, zeroconf: Zeroconf, service_type: str, name: str) -> None:
        self.owner._resolve_service(service_type, name)

    def update_service(self, zeroconf: Zeroconf, service_type: str, name: str) -> None:
        self.owner._resolve_service(service_type, name)

    def remove_service(self, zeroconf: Zeroconf, service_type: str, name: str) -> None:
        self.owner._remove_service(name)


class DesktopPeerNode:
    def __init__(
        self,
        directory: Path,
        lan_ip: str,
        inject_callback: Callable[[str], None],
        approval_callback: Callable[[PeerInfo, str, bool], bool],
        peers_callback: Callable[[list[PeerInfo]], None],
        receiving_callback: Callable[[], bool],
    ):
        self.identity = PeerIdentity(directory)
        self.trust = TrustStore(directory / "peer-trust.json")
        self.lan_ip = lan_ip
        self.inject_callback = inject_callback
        self.approval_callback = approval_callback
        self.peers_callback = peers_callback
        self.receiving_callback = receiving_callback
        self.server_context = self.identity.ssl_context(server=True)
        self.client_context = self.identity.ssl_context(server=False)
        self.server_socket: socket.socket | None = None
        self.port = 0
        self.running = threading.Event()
        self.workers = ThreadPoolExecutor(max_workers=4, thread_name_prefix="voicebridge-peer")
        self.connection_slots = threading.BoundedSemaphore(8)
        self.pairing_lock = threading.Lock()
        self.peer_lock = threading.RLock()
        self.peers: dict[str, PeerInfo] = {}
        self.service_names: dict[str, str] = {}
        self.failures: dict[str, int] = {}
        self.seen_messages: dict[str, float] = {}
        self.zeroconf: Zeroconf | None = None
        self.browser: ServiceBrowser | None = None
        self.service_info: ServiceInfo | None = None
        self.udp_socket: socket.socket | None = None

    def start(self) -> None:
        if self.running.is_set():
            return
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", 0))
        server.listen(8)
        server.settimeout(1)
        self.server_socket = server
        self.port = server.getsockname()[1]
        self.running.set()
        discovery_errors = []
        try:
            self._start_discovery()
        except Exception as error:
            discovery_errors.append(error)
        try:
            self._start_udp_discovery()
        except Exception as error:
            discovery_errors.append(error)
        if len(discovery_errors) == 2:
            self.running.clear()
            server.close()
            raise RuntimeError("mDNS 和 UDP 自动发现均启动失败") from discovery_errors[0]
        threading.Thread(target=self._accept_loop, daemon=True, name="voicebridge-peer-accept").start()
        threading.Thread(target=self._heartbeat_loop, daemon=True, name="voicebridge-peer-heartbeat").start()

    def stop(self) -> None:
        self.running.clear()
        if self.browser is not None:
            self.browser.cancel()
        if self.zeroconf is not None:
            if self.service_info is not None:
                try:
                    self.zeroconf.unregister_service(self.service_info)
                except Exception:
                    pass
            self.zeroconf.close()
        self.browser = None
        self.zeroconf = None
        if self.udp_socket is not None:
            try:
                self.udp_socket.close()
            except OSError:
                pass
        self.udp_socket = None
        if self.server_socket is not None:
            try:
                self.server_socket.close()
            except OSError:
                pass
        self.server_socket = None
        self.workers.shutdown(wait=False, cancel_futures=True)

    def refresh_network(self, lan_ip: str) -> None:
        if lan_ip == self.lan_ip:
            return
        self.lan_ip = lan_ip
        if self.zeroconf is not None:
            if self.service_info is not None:
                try:
                    self.zeroconf.unregister_service(self.service_info)
                except Exception:
                    pass
            self._register_service()
        if self.udp_socket is not None:
            try:
                self.udp_socket.close()
            except OSError:
                pass
            self.udp_socket = None
            try:
                self._start_udp_discovery()
            except OSError:
                pass

    def snapshot(self) -> list[PeerInfo]:
        with self.peer_lock:
            return sorted(self.peers.values(), key=lambda item: item.name.casefold())

    def pair(self, peer_id: str) -> dict:
        peer = self._peer(peer_id)
        nonce = base64.b64encode(secrets.token_bytes(24)).decode("ascii").rstrip("=")
        code = pairing_code(self.identity.fingerprint, peer.fingerprint, nonce)
        if not self.approval_callback(peer, code, False):
            return self._result("unauthorized", "已取消配对")
        secret = base64.b64encode(secrets.token_bytes(32)).decode("ascii").rstrip("=")
        request = self._signed({
            "type": "PAIR_REQUEST", "version": PROTOCOL_VERSION,
            "peerId": self.identity.id, "peerName": self.identity.name,
            "deviceType": "computer", "nonce": nonce, "secret": secret,
        })
        response = self._transact(peer, request)
        if response.get("ok") is True and response.get("code") == code:
            self.trust.put(peer.id, peer.fingerprint, secret)
            self._update_peer(peer.id, trusted=True, state="connecting")
            return self._result("injected", "配对成功")
        return response

    def send_text(self, peer_id: str, text: str) -> dict:
        if not text or len(text.encode("utf-8")) > MAX_TEXT_BYTES:
            return self._result("invalid", "内容为空或超过 100 KiB")
        return self._authenticated_request(self._peer(peer_id), "TEXT_PUSH", text)

    def _start_discovery(self) -> None:
        self.zeroconf = Zeroconf()
        self._register_service()
        self.browser = ServiceBrowser(self.zeroconf, SERVICE_TYPE, _DiscoveryListener(self))

    def _register_service(self) -> None:
        if self.zeroconf is None:
            return
        properties = {
            b"id": self.identity.id.encode(), b"name": self.identity.name.encode("utf-8"),
            b"type": b"computer", b"fp": self.identity.fingerprint.encode(), b"v": b"2",
        }
        info = ServiceInfo(
            SERVICE_TYPE, f"VoiceBridge-{self.identity.id[:8]}.{SERVICE_TYPE}",
            addresses=[socket.inet_aton(self.lan_ip)], port=self.port, properties=properties,
        )
        self.zeroconf.register_service(info, allow_name_change=True)
        self.service_info = info

    def _start_udp_discovery(self) -> None:
        multicast = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        multicast.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        multicast.bind(("", DISCOVERY_PORT))
        membership = socket.inet_aton(DISCOVERY_GROUP) + socket.inet_aton(self.lan_ip)
        multicast.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
        multicast.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
        multicast.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(self.lan_ip))
        multicast.settimeout(1)
        self.udp_socket = multicast
        threading.Thread(target=self._udp_receive_loop, args=(multicast,), daemon=True, name="voicebridge-udp-receive").start()
        threading.Thread(target=self._udp_announce_loop, args=(multicast,), daemon=True, name="voicebridge-udp-announce").start()

    def _udp_announce_loop(self, multicast: socket.socket) -> None:
        target = (DISCOVERY_GROUP, DISCOVERY_PORT)
        while self.running.is_set() and multicast is self.udp_socket:
            message = json.dumps({
                "app": "voicebridge", "version": PROTOCOL_VERSION,
                "id": self.identity.id, "name": self.identity.name, "deviceType": "computer",
                "fingerprint": self.identity.fingerprint, "port": self.port,
            }, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            if len(message) <= MAX_BEACON_BYTES:
                try:
                    multicast.sendto(message, target)
                except OSError:
                    return
            time.sleep(5)

    def _udp_receive_loop(self, multicast: socket.socket) -> None:
        while self.running.is_set() and multicast is self.udp_socket:
            try:
                payload, address = multicast.recvfrom(MAX_BEACON_BYTES + 1)
            except socket.timeout:
                continue
            except OSError:
                return
            if len(payload) > MAX_BEACON_BYTES:
                continue
            try:
                data = json.loads(payload.decode("utf-8"))
                peer_id = data["id"]
                fingerprint = data["fingerprint"]
                port = int(data["port"])
            except (KeyError, TypeError, ValueError, UnicodeError, json.JSONDecodeError):
                continue
            if data.get("app") != "voicebridge" or data.get("version") != PROTOCOL_VERSION or peer_id == self.identity.id:
                continue
            if not isinstance(peer_id, str) or not isinstance(fingerprint, str) or len(fingerprint) != 64 or port not in range(1, 65536):
                continue
            trusted = self.trust.get(peer_id)
            is_trusted = bool(trusted and hmac.compare_digest(trusted.get("fingerprint", ""), fingerprint))
            with self.peer_lock:
                existing = self.peers.get(peer_id)
                state = "connected" if existing and existing.state == "connected" else ("connecting" if is_trusted else "unpaired")
                self.peers[peer_id] = PeerInfo(
                    peer_id, str(data.get("name") or "设备")[:40], str(data.get("deviceType") or "phone"),
                    address[0], port, fingerprint, is_trusted, state, time.time(),
                    existing.last_heartbeat if existing else 0,
                )
            self._notify_peers()

    def _resolve_service(self, service_type: str, name: str) -> None:
        if self.zeroconf is None:
            return
        info = self.zeroconf.get_service_info(service_type, name, timeout=2500)
        if info is None or not info.addresses:
            return
        props = {key.decode(): value.decode("utf-8") for key, value in info.properties.items()}
        peer_id = props.get("id", "")
        if not peer_id or peer_id == self.identity.id or props.get("v") != "2":
            return
        fingerprint = props.get("fp", "")
        trusted = self.trust.get(peer_id)
        is_trusted = bool(trusted and hmac.compare_digest(trusted.get("fingerprint", ""), fingerprint))
        host = socket.inet_ntoa(info.addresses[0])
        peer = PeerInfo(
            peer_id, props.get("name", "Android 设备")[:40], props.get("type", "phone"),
            host, info.port, fingerprint, is_trusted,
            "connecting" if is_trusted else "unpaired", time.time(), 0,
        )
        with self.peer_lock:
            existing = self.peers.get(peer_id)
            if existing and existing.state == "connected":
                peer = replace(peer, state="connected", last_heartbeat=existing.last_heartbeat)
            self.peers[peer_id] = peer
            self.service_names[name] = peer_id
        self._notify_peers()

    def _remove_service(self, name: str) -> None:
        with self.peer_lock:
            peer_id = self.service_names.pop(name, None)
            if peer_id in self.peers:
                self.peers[peer_id] = replace(self.peers[peer_id], state="retrying")
        self._notify_peers()

    def _accept_loop(self) -> None:
        while self.running.is_set():
            try:
                client, _address = self.server_socket.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            if not self.connection_slots.acquire(blocking=False):
                client.close()
                continue
            self.workers.submit(self._handle_client_safely, client)

    def _handle_client_safely(self, client: socket.socket) -> None:
        try:
            self._handle_client(client)
        except Exception:
            pass
        finally:
            self.connection_slots.release()

    def _handle_client(self, client: socket.socket) -> None:
        with client, self.server_context.wrap_socket(client, server_side=True) as secure:
            secure.settimeout(SOCKET_TIMEOUT)
            line = self._read_line(secure, MAX_TEXT_BYTES + 8192)
            request = json.loads(line.decode("utf-8"))
            response = self._process_request(request)
            secure.sendall(json.dumps(response, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n")

    def _process_request(self, request: dict) -> dict:
        if request.get("version") != PROTOCOL_VERSION:
            return self._result("invalid", "协议版本不兼容")
        fingerprint = self._verified_fingerprint(request)
        peer_id = request.get("peerId")
        if not fingerprint or not isinstance(peer_id, str) or not peer_id:
            return self._result("unauthorized", "客户端证书签名无效")
        request_type = request.get("type")
        if request_type == "PAIR_REQUEST":
            return self._accept_pair(request, peer_id, fingerprint)
        trusted = self.trust.get(peer_id)
        if not trusted or not hmac.compare_digest(trusted.get("fingerprint", ""), fingerprint):
            return self._result("unauthorized", "设备尚未配对或证书已变化")
        message_id = request.get("messageId", "")
        text = request.get("text", "")
        supplied = request.get("auth", "")
        expected = message_auth(trusted["secret"], message_id, text)
        if not isinstance(message_id, str) or len(message_id) not in range(16, 81) or not hmac.compare_digest(supplied, expected):
            return self._result("unauthorized", "消息认证失败")
        replay_key = f"{peer_id}:{message_id}"
        with self.peer_lock:
            if replay_key in self.seen_messages:
                return self._result("invalid", "重复消息已拒绝")
            self.seen_messages[replay_key] = time.time()
            while len(self.seen_messages) > 256:
                self.seen_messages.pop(next(iter(self.seen_messages)))
        self._update_peer(peer_id, state="connected", last_heartbeat=time.time())
        if request_type == "PING":
            return {"type": "PONG", "version": PROTOCOL_VERSION, "ok": True}
        if request_type != "TEXT_PUSH":
            return self._result("invalid", "未知请求")
        if not self.receiving_callback():
            return self._result("paused", "目标设备已暂停接收")
        if not text or len(text.encode("utf-8")) > MAX_TEXT_BYTES:
            return self._result("invalid", "内容为空或超过 100 KiB")
        try:
            self.inject_callback(text)
        except Exception:
            return self._result("unsupported_editor", "电脑当前窗口拒绝了文字写入")
        return self._result("injected", "")

    def _accept_pair(self, request: dict, peer_id: str, fingerprint: str) -> dict:
        if not self.pairing_lock.acquire(blocking=False):
            return self._result("unauthorized", "已有配对请求正在确认")
        try:
            peer_name = request.get("peerName", "")
            nonce = request.get("nonce", "")
            secret = request.get("secret", "")
            if not isinstance(peer_name, str) or not peer_name or len(nonce) not in range(16, 129) or len(secret) not in range(32, 129):
                return self._result("invalid", "配对参数无效")
            peer = PeerInfo(peer_id, peer_name[:40], request.get("deviceType", "phone"), "", 0, fingerprint)
            code = pairing_code(self.identity.fingerprint, fingerprint, nonce)
            if not self.approval_callback(peer, code, True):
                return self._result("unauthorized", "目标设备拒绝配对")
            self.trust.put(peer_id, fingerprint, secret)
            self._update_peer(peer_id, trusted=True, state="connecting")
            return {"type": "PAIR_RESULT", "version": PROTOCOL_VERSION, "ok": True, "code": code}
        finally:
            self.pairing_lock.release()

    def _authenticated_request(self, peer: PeerInfo, request_type: str, text: str) -> dict:
        trusted = self.trust.get(peer.id)
        if not trusted or not hmac.compare_digest(trusted.get("fingerprint", ""), peer.fingerprint):
            return self._result("unauthorized", "设备尚未配对或证书已变化")
        message_id = str(uuid.uuid4())
        request = self._signed({
            "type": request_type, "version": PROTOCOL_VERSION,
            "peerId": self.identity.id, "messageId": message_id, "text": text,
            "auth": message_auth(trusted["secret"], message_id, text),
        })
        try:
            return self._transact(peer, request)
        except Exception:
            return self._result("unreachable", "无法连接目标设备")

    def _transact(self, peer: PeerInfo, request: dict) -> dict:
        with socket.create_connection((peer.host, peer.port), timeout=SOCKET_TIMEOUT) as raw:
            with self.client_context.wrap_socket(raw, server_hostname=None) as secure:
                secure.settimeout(SOCKET_TIMEOUT)
                certificate = secure.getpeercert(binary_form=True)
                fingerprint = hashlib.sha256(certificate).hexdigest()
                if not hmac.compare_digest(fingerprint, peer.fingerprint):
                    raise ssl.SSLError("certificate fingerprint mismatch")
                secure.sendall(json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n")
                return json.loads(self._read_line(secure, 16_384).decode("utf-8"))

    def _signed(self, message: dict) -> dict:
        message["fingerprint"] = self.identity.fingerprint
        message["certificate"] = self.identity.encoded_certificate
        message["signature"] = self.identity.sign(canonical_payload(message))
        return message

    @staticmethod
    def _verified_fingerprint(message: dict) -> str | None:
        try:
            certificate = x509.load_der_x509_certificate(base64.b64decode(message["certificate"]))
            now = datetime.now(timezone.utc)
            if not (certificate.not_valid_before_utc <= now <= certificate.not_valid_after_utc):
                return None
            fingerprint = certificate.fingerprint(hashes.SHA256()).hex()
            if not hmac.compare_digest(fingerprint, message.get("fingerprint", "")):
                return None
            certificate.public_key().verify(
                _padded_base64(message["signature"]), canonical_payload(message), ec.ECDSA(hashes.SHA256()),
            )
            return fingerprint
        except Exception:
            return None

    @staticmethod
    def _read_line(secure: ssl.SSLSocket, maximum: int) -> bytes:
        data = bytearray()
        while len(data) <= maximum:
            chunk = secure.recv(min(4096, maximum + 1 - len(data)))
            if not chunk:
                break
            newline = chunk.find(b"\n")
            if newline >= 0:
                data.extend(chunk[:newline])
                return bytes(data)
            data.extend(chunk)
        raise ValueError("message missing newline or too large")

    def _heartbeat_loop(self) -> None:
        while self.running.is_set():
            time.sleep(5)
            if not self.running.is_set():
                return
            for peer in self.snapshot():
                if not peer.trusted:
                    continue
                result = self._authenticated_request(peer, "PING", "")
                if result.get("type") == "PONG" and result.get("ok") is True:
                    self.failures[peer.id] = 0
                    self._update_peer(peer.id, state="connected", last_heartbeat=time.time())
                else:
                    failures = self.failures.get(peer.id, 0) + 1
                    self.failures[peer.id] = failures
                    if failures >= 3:
                        self._update_peer(peer.id, state="retrying")

    def _peer(self, peer_id: str) -> PeerInfo:
        with self.peer_lock:
            peer = self.peers.get(peer_id)
        if peer is None:
            raise KeyError("目标设备不在当前局域网")
        return peer

    def _update_peer(self, peer_id: str, **values) -> None:
        with self.peer_lock:
            if peer_id in self.peers:
                self.peers[peer_id] = replace(self.peers[peer_id], **values)
        self._notify_peers()

    def _notify_peers(self) -> None:
        self.peers_callback(self.snapshot())

    @staticmethod
    def _result(code: str, detail: str) -> dict:
        return {"type": "TEXT_RESULT", "version": PROTOCOL_VERSION, "ok": code == "injected", "code": code, "detail": detail}
