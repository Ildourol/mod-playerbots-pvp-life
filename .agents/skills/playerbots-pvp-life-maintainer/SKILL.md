---
name: playerbots-pvp-life-maintainer
description: Maintain, review, debug, plan, and extend mod-playerbots-pvp-life (Playerbots PvP Life & World Skirmishes) across AzerothCore C++ hooks, database schemas, and configuration settings.
---

# Playerbots PvP Life & World Skirmishes Maintainer Skill (`playerbots-pvp-life-maintainer`)

This skill provides operational workflows, engineering checklists, and diagnostic runbooks for maintaining and extending `mod-playerbots-pvp-life`.

## Core Responsibilities
- **Component**: Playerbots PvP Life & World Skirmishes
- **Description**: Spawns autonomous rival faction playerbots into contested world zones, orchestrating emergent world PvP clashes and territory defense.
- **Target Platform**: AzerothCore (WotLK 3.3.5a, Build 12340, C++17)

---

## Architectural Invariants
- World PvP skirmishes must respect zone faction guard rules and graveyard distances.
- PvP bots must disengage if non-PvP flagged players are nearby to prevent griefing.
- Combat threat and target evaluation must update at regular AI intervals without spiking world ticks.
- **Zero Blocking I/O**: The AzerothCore world loop is sacred. Never add blocking I/O, sleep loops, or external synchronous network requests to the world thread.
- **Pointer Safety**: Never hold raw entity pointers across game ticks. Re-resolve entities freshly using numeric GUIDs.
- **Idempotent SQL**: All database migrations must be 100% idempotent with `CREATE TABLE IF NOT EXISTS` and safe column checks.

---

## Maintenance Runbook

### 1. Preflight Inspection
1. Verify configuration files in `conf/` and `Server/bin/configs/modules/`.
2. Inspect registered script hooks (`PlayerScript`, `WorldScript`, `UnitScript`, etc.).
3. Verify that database tables match schema declarations in `data/sql/`.

### 2. Issue Investigation & Tracking
1. Consult [`docs/ISSUES.md`](file:///C:/Users/Admin/AntigravityProfiles/Projects%20Azerothcore/Azerothcore%20modules/mod-playerbots-pvp-life/docs/ISSUES.md) for known issues and resolutions.
2. Update only the issue summary and direct module-record link in [COMMANDER/ISSUES.md](<../../../../COMMANDER/ISSUES.md>); keep technical records, fix instructions, and plans inside this module.
3. Follow the 9-point technical issue template strictly.

### 3. Verification & Safety Checks
- **MySQL Testing**: Perform all test compilation and general tests (unit tests, integration tests, SQL migrations, and runtime verification) against MySQL, the default database engine for AzerothCore.
- Verify thread safety of all member buffers and static registries using std::mutex or std::shared_mutex where required.
- Validate skill integrity:
  ```bash
  python "C:\Users\Admin\.codex\skills\.system\skill-creator\scripts\quick_validate.py" ".agents/skills/playerbots-pvp-life-maintainer"
  ```
