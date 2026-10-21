# Extended system-module state tracking

The extended sysmodule entry points use the existing HLE module tracker:

| Entry point | NID | Accepted IDs |
| --- | --- | --- |
| `cellSysmoduleIsLoadedEx` | `0x70E319B5` | Internal namespace `0xF000..0xF056` |
| `cellSysmoduleLoadModuleEx` | `0x1A267F98` | 48 supported IDs from the Ex allowlist |
| `cellSysmoduleUnloadModuleEx` | `0xFD0E1CAC` | All 51 IDs in the Ex allowlist |

Each takes one guest `u16` module ID. Upper argument bits are outside that
integer ABI value; additional PPU registers are not pointer or output arguments.
Disallowed IDs return `CELL_SYSMODULE_ERROR_UNKNOWN` (`0x80012002`) without
changing state. Query returns `CELL_OK` for a loaded module and
`CELL_SYSMODULE_ERROR_UNLOADED` (`0x80012003`) otherwise. Loads preserve the
existing tracker's idempotence. Unloading a permitted loaded module returns
`CELL_OK`; repeating the unload returns UNLOADED. Core `0x0044` and internal
`0xF044` remain distinct tracker entries.

The shared Ex allowlist includes `0xF020`, `0xF024`, and `0xF03E` for unloading.
Loading those IDs returns UNKNOWN because their process-PARAM.SFO permission
checks are not implemented. Unload does not perform a load-time permission check.

These APIs perform no guest-memory writes. They do not load or unload a firmware
PRX, execute finalizers, manage dependency reference counts, free module images,
select memory containers, or establish that a tracked module's APIs are implemented.
The existing tracker's concurrency behavior is unchanged.

The accepted-ID policy and integer ABI follow RPCS3's
[cellSysmodule.cpp](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSysmodule.cpp),
including `LOADABLE_INTERNAL_MODULES_EX` and its internal load/unload checks.
Error values follow
[cellSysmodule.h](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSysmodule.h).
All three NIDs are computed from their symbols by the normal SDK generator.

Run the asset-free regressions:

```sh
python tools/test_sysmodule_is_loaded_ex.py
python tools/test_sysmodule_load_module_ex.py
python tools/test_sysmodule_unload_module_ex.py
```

The query/load fixtures check state transitions, bounds, idempotence, and rejected
IDs retaining their state. The combined fixture compiles the production sysmodule
implementation, generated NID table, and production PPU HLE bridge. It checks every
internal ID against a literal policy table, query/load/unload transitions, namespace
separation, upper argument bits, and unused argument registers containing invalid
addresses. Guest-memory canaries allow only the bridge's ABI-required TOC linkage
write. All runners bound execution and skip standalone compilation on Windows.
The existing macOS/Linux `tools/test_*.py` discovery runs them without another CI step.
