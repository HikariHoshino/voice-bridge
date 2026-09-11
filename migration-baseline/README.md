# Windows Native Migration Baseline

This directory is an immutable working-tree snapshot used only to verify the
native migration. Do not edit these copied files.

- Git worktree: `codex/windows-native-refactor`
- Committed source recovery point: `5e37383710ea1e7a26552ffd15c2e7d79080dac7`
- Source working tree: `D:/Coding/Codex/Voice Bridge`
- Snapshot date: 2026-09-11 (Asia/Shanghai)
- Project `AGENTS.md`: not present; the UTF-8 instructions supplied in the task
  are the governing project instructions.

The source working tree was dirty before this migration and was not modified:

- `app.py`: modified, SHA-256
  `3E14135AF5A0C437BECB081F3981404AF1C4A90ADD4CC1AE78A421BC30FFD432`
- `test_app.py`: modified, SHA-256
  `F5CFDD2AE61CE208AADF344C89DB6C1C215F81D193307887C2A8A8D21B75441C`
- `mobile.html`: SHA-256
  `75864383FA890BD5604F217E4678B44B8ED4042A49FB716CBC13CF748A8D1B47`
- `assets/voice-bridge.ico`: SHA-256
  `0C7F65D6A8358383BB5B5F1AEA2ED398C1B2DD6ADDC1CD256FD99E7B05FE18BC`
- `assets/voice-bridge.png`: SHA-256
  `12DE18B061B2EAE251C4C0128C354D08A1F275E172B812F89CED84DDB35437FB`
- `assets/voice-bridge.svg`: SHA-256
  `6B830D1EAF90BA65B0A98A6638774FC8F37715EBDD17C30F70F581C085DA6CD9`

Verification performed from `working-tree/` with the existing Python 3.13
virtual environment:

```powershell
python -B -m unittest -v
```

Result: 61 tests passed. The original source hashes and Git status were checked
again after the copy and were unchanged.
