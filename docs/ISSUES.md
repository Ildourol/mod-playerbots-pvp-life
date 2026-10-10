# mod-playerbots-pvp-life Issues & Diagnostics Log

> **Location:** `mod-playerbots-pvp-life/docs/ISSUES.md`
> **Source Module:** [`mod-playerbots-pvp-life`](file:///C:/Users/Admin/AntigravityProfiles/Projects%20Azerothcore/Azerothcore%20modules/mod-playerbots-pvp-life)  
> **Commander Navigator:** [`COMMANDER/README.md`](file:///C:/Users/Admin/AntigravityProfiles/Projects%20Azerothcore/Azerothcore%20modules/COMMANDER/README.md)


---

## 1. Module Issues Summary

| Issue ID | Title | Severity | Status | Affected Component | Resolved / Documented In |
|---|---|---|---|---|---|
| **[MPPL-001]** | Missing Database Table `pvp_life_zone` on World Server Boot | Medium | **RESOLVED** | World Database (`acore_world`) / Schema Initializer | `data/sql/manual/world_pvp_life.sql` |

---

## 2. Technical Issue Details

<a id="mppl-001"></a>

### [MPPL-001] Missing Database Table `pvp_life_zone` on World Server Boot
* **Severity**: Medium (Feature Degraded / Disabled at Startup)
* **Component**: Database Schema Initializer / `mod_playerbots_pvp_life.cpp`
* **Status**: **RESOLVED** (2026-10-09)
* **Affected Files & Locations**:
  - `modules/mod-playerbots-pvp-life/data/sql/manual/world_pvp_life.sql`
  - Table: `acore_world.pvp_life_zone`
* **Symptoms & Terminal Log Traces**:
  During worldserver initialization:
  ```text
  [PvPLife] Missing WORLD table pvp_life_zone. Apply data/sql/manual/world_pvp_life.sql
  [PvPLife] enable=0 database=0 alwaysWorld=1 duel=1 FTH=1 FTA=1 botChat=1 announce=0 prefix='rndbot' respectActivity=0 partialTeams=1 minimumPerSide=1
  ```
* **Root Cause Analysis**:
  The module relies on the table `pvp_life_zone` in the `acore_world` database to configure world PvP combat zones and bot engagement settings. When the table is absent, the module gracefully disables itself (`enable=0 database=0`).
* **Resolution**:
  Import `data/sql/manual/world_pvp_life.sql` into the `acore_world` database using the MySQL CLI or database manager when outdoor PvP bot behaviors are desired.
