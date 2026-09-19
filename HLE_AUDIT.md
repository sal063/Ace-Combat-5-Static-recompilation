# HLE audit — Ace Combat 5, SLUS_208.51

Date: 2026-09-19

## Repair status

The four confirmed findings below are now resolved in this release tree:

| Finding | Resolution |
|---|---|
| F1 | The live S-command RPC function 3 reports PS2 DVD (`0x14`) when a disc is mounted and no disc (`0`) otherwise. It no longer reaches the generic success reply. |
| F2 | A bounded page allocator reclaims freed blocks, rejects invalid/interior/double frees, supports first/last/fixed-address allocation, and computes total/largest free space from allocation state. |
| F3 | Vibration profile reports zero supported motors. Host rumble remains unimplemented and is no longer advertised. |
| F4 | Pad Init/End clear all 16 sockets and return 1; valid DeleteSocket returns 1 and releases its slot. |

Signed NUFILE relative seeks and byte-exact RPC zero filling were also repaired. The TOC, asynchronous timing, raw IOP destination and hardware audio fidelity items remain explicitly unverified or incomplete; they are not claimed fixed by these changes. The finding descriptions below record the pre-repair audit evidence.

## Scope and evidence

Compared the 73 applied EE override bindings in `config/report.json` with the original functions in the open IDA database, then followed selected game callers and native RPC handlers. The inventory below records every binding inspected. Names are runtime binding names; original addresses identify the evidence independently of those names.

This is a static contract audit, with regression execution for the audio port. It is not a claim of hardware equivalence or complete coverage of kernel syscalls, every IOP module, every memory-card operation, or every mission. Direct xrefs do not include all indirect calls. Decompiled code, extracted game data, generated game sources and test executables were kept outside this release tree.

## Port completed

The three changed source files contain no C/C++ comments:

- `runtime/src/ps2_hle_iop.c`: clamp signed NUSNDSTR volumes, preserve phase in stereo/surround, apply the original absolute-value mono fold, and encode SPU2 volume registers without accidentally selecting sweep mode. Applies to tone attributes and stream volume commands.
- `runtime/src/ps2_spu2.c`: interpolate fractional sample positions and preserve every pitch step when advancing across ADPCM blocks.
- `runtime/src/ps2_nustream.c`: interpolate fractional stream positions, including block boundaries.

The signed-volume contract is supported by EE packet producers at `0x0034F4A8` and `0x00354458`, and the previously inspected NUSOUND.IRX volume handling: tone clamp/mono/mask at module offsets `0x05DC–0x0770`, stream equivalents at `0x0A34–0x0B40`. Interpolation is linear; it does not reproduce the hardware Gaussian filter.

No FMV behavior change or FMV diagnostic module was ported.

## Findings

### F1 — P2: the live disc-type query receives the generic success value

[ps2_hle_cdvd.c:210](runtime/src/ps2_hle_cdvd.c#L210)

The original function at `0x0033F698` asks the S-command RPC service for function 3 and returns its four-byte reply. Its helper at `0x0033E700` binds SID `0x80000593`. The native handler writes 1 for every generic request. Although `hle_sceCdGetDiskType` exists and returns `0x14`, there is no applied override at `0x0033F698`.

The game caller at `0x0027F2D0` explicitly branches on `0x12` and `0x14` to initialize its CD/DVD mode field. A reply of 1 skips both assignments and the caller passes the existing field to `sceCdMmode`. This is a confirmed wrong return value and a bypassed initialization branch. A visible gameplay failure has not been reproduced; the current native read path mostly ignores the media mode.

Recommended repair: bind the existing HLE at the verified address and/or implement function 3 in the S-command service with consistent disc-present/media semantics. Verify the generated binding and the caller's initialized mode.

### F2 — P2: successful IOP frees never reclaim storage

[ps2_hle_sif.c:209](runtime/src/ps2_hle_sif.c#L209)

The originals at `0x00335CB8` and `0x00335C40` send a free request to IOP sysmem. Native free functions return success without releasing anything, while allocations monotonically advance `iop_heap_next`.

This is used by the sound system: allocation wrappers `0x00359E50` / `0x00359EE8` retain the heap pointer, and `0x00359F58` frees and clears it during the owned-heap branch of `nuSoundQuit` at `0x00354A70`. Repeating that initialization/teardown path permanently consumes space until later allocation fails. This does not establish that every mission transition runs that path.

`AllocSysMemory` also lacks a capacity check and ignores its allocation mode/address; this adds a conditional out-of-range allocation risk. No observed request was shown to trigger that separate condition.

Recommended repair: maintain allocation/free bookkeeping, honor the supported allocation modes, validate arithmetic and bounds, and test repeated sound heap lifetimes.

### F3 — P2: vibration is advertised but output requests are discarded

[ps2_hle_iop.c:60](runtime/src/ps2_hle_iop.c#L60); [ps2_hle_pad.c:398](runtime/src/ps2_hle_pad.c#L398)

`hle_sceVibGetProfile` advertises both motors with profile byte 3. The game's pad update at `0x0032AD00` uses that profile and calls the original output routine at `0x0033C1B8`. That routine builds command `0x0103400B`, then `0x0033B4A0` submits DBC function `0x8000131B`.

The DBC handler only implements the version reply; output payloads receive an empty successful response. No host rumble implementation was found in the release runtime. Thus this path reports support without producing vibration. This concerns controller feedback, not the audio channel defect.

Recommended repair: decode the output command and map its motors to the host controller API, or accurately report unsupported vibration until implemented.

### F4 — P3: pad lifecycle return values and cleanup differ

[ps2_hle_pad.c:197](runtime/src/ps2_hle_pad.c#L197)

Original Init (`0x0033B880`), successful DeleteSocket (`0x0033BA30`) and End (`0x0033B8C0`) return 1. Native equivalents return 0. Original Init clears the socket table; End releases remaining sockets, whereas native Init/End only toggle initialization state.

The inspected game setup/teardown callers (`0x0032AC08`, `0x0032ACB0`) ignore these return values and explicitly delete their sockets before End, so this is a compatibility defect with limited demonstrated impact on the normal game path. A caller relying on End cleanup can retain stale sockets across reinitialization.

Recommended repair: match success returns and reset/release socket state, retaining normal controller connection behavior.

## Additional gaps and conditional risks

- **TOC fidelity:** [ps2_hle_cdvd.c:176](runtime/src/ps2_hle_cdvd.c#L176). Original `0x0033F338` copies 1024 or 2064 bytes according to the service reply. The HLE always fabricates 1024 bytes, mostly zero. Game caller `0x0027F448` reads byte 17 as BCD and consequently stores zero. The correct value for this particular DVD was not established from hardware, and no downstream failure was demonstrated; this is a verified stub and a validation gap, not proof of a broken mission.
- **NUFILE relative seek (repaired):** [ps2_hle_iop.c:236](runtime/src/ps2_hle_iop.c#L236). Client `0x0035C36C` places the offset at packet +12 and whence at +16. Native arithmetic treats the offset as unsigned. A negative offset for current/end-relative seeking moves to EOF after clamping instead of backwards. No negative-offset game call was established, so this is conditional compatibility risk rather than an observed music failure.
- **RPC completion timing:** native NOWAIT completes inline and CheckStat always reports idle. Original `0x00331C88` uses asynchronous completion. No concrete post-submit write clobbering a callback result was established. Preserve this as a timing/reentrancy risk, not a demonstrated deadlock.
- **RPC queue registration:** `0x00331EC0` links queues into the original global queue list; the HLE initializes fields only. NUFILE receiver thread `0x0035A4AC` still calls original registration/receive routines, but native NUFILE directly delivers its own completions. That bypass must be considered before treating the missing list insertion as an active failure.
- **Raw IOP destinations:** ReadIOPm (`0x0033F1E0`) shares the EE read implementation. The native guest page table can represent synthetic IOP addresses. Compatibility of raw original IOP offsets is unproven; a call using an unconverted low IOP offset could target EE memory. The game dispatcher at `0x0027F6A8` selects the IOP variant using request flag bit 0; no offending destination was demonstrated.
- **Stub success replies:** unknown RPC services, module loading and generic CDVD requests often report success without implementing their full outputs. F1 is a concrete game dependency found within that broader pattern. ELF/module output parameters, real IOP reset semantics and error paths need dedicated coverage before claiming SDK compatibility.
- **Reply bounds (repaired for the audited zero-fill loops):** several zero-fill loops write whole words for arbitrary reply byte counts. A non-multiple-of-four length can overwrite up to three bytes. No such request was established in the inspected game call paths.
- **Audio fidelity:** effect-send routing state does not amount to a complete SPU2 reverb implementation; linear interpolation is still an approximation. The audio port addresses signed channel volume and sample stepping, not every source of PS2 sound differences.

## Checks that did not justify a finding

The pad read layout is 18 bytes and the profile is four bytes, matching the inspected game consumer. Stable state 1 is the value tested by that consumer. The internal GetSide/GetSide2 stubs are normally called through other overridden pad functions, so their zero return was not counted as a demonstrated null dereference.

NUFILE seek packet positions match the original client; the concern is signed arithmetic, not an assumed alternate packet layout. Synchronous native read completion was not itself counted as a defect without a demonstrated caller dependency on asynchronous timing.

## Validation

The repair adds `--hle-test` for allocator reuse/exhaustion/fragmentation and invalid frees, fixed/high allocation, pad lifecycle and socket capacity, unsupported motor reporting, exact reply buffer bounds, disc-type RPC results, and backward NUFILE seeks. Run with `--disc <game.iso>` to include disc-backed cases.

A fresh CMake/Ninja build of the release runtime and external generated game sources completed successfully. Its executable passed the disc-backed HLE and audio suites. Existing signedness warnings in the unchanged video source remain. All changed C/C++ files were verified comment-free.

Repair validation passed for `--hle-test`, `--hle-test --disc <game.iso>` and the disc-backed audio suite. An integration executable using all newly built release runtime objects and existing generated objects completed a headless 180-vblank game boot with 645 RPC calls and zero unserviced calls.

The following describes validation of the earlier audio-only port:

Compiled the three changed files from this release directory with the existing GCC flags, then linked them against the existing development build's other runtime/generated objects. This validates the ported translation units and their integration, but is not a clean rebuild of the complete sanitized tree.

Executed `--spu2-test`, then `--spu2-test --disc C:\PS2Rec\game.iso`. Both completed successfully. Checks passed for signed stereo/surround/mono volume conversion and clamping, fractional interpolation, high-pitch block advancement, stream stereo/position/pause/EOF/loop/stop, NUSNDSTR address boundaries and effect-send/key separation, and NUSOUND start modes/packet handling. Five radio archive samples decoded to EOF in the disc-backed test.

`git diff --check` passed. A lexer-aware scan found no comments in the three modified source files, and a token comparison confirmed that they match the development audio fixes after removing comments. The subsequent HLE repairs are described in Repair status above. No listening test or hardware comparison was performed in this audit.

## Binding inventory

All entries below were compared with original function bodies in IDA. Group notes describe review scope, not a blanket assertion of equivalence. The xref count is code xref sites found by IDA and is not a runtime execution count.

| Original EE address | Native binding | Code xref sites | Review area |
|---|---|---:|---|
| `0x0032FF68` | `hle_write` | 2 | Debug/console stub contract |
| `0x00331320` | `hle_sceSifInitRpc` | 13 | SIF/RPC layout, completion and bypasses |
| `0x003314C0` | `hle_sceSifExitRpc` | 1 | SIF/RPC layout, completion and bypasses |
| `0x00331800` | `hle_sceSifGetOtherData` | 0 | SIF/RPC layout, completion and bypasses |
| `0x00331AB8` | `hle_sceSifBindRpc` | 18 | SIF/RPC layout, completion and bypasses |
| `0x00331C88` | `hle_sceSifCallRpc` | 120 | SIF/RPC layout, completion and bypasses |
| `0x00331E80` | `hle_sceSifCheckStatRpc` | 12 | SIF/RPC layout, completion and bypasses |
| `0x00331EC0` | `hle_sceSifSetRpcQueue` | 1 | SIF/RPC layout, completion and bypasses |
| `0x00335AC8` | `hle_sceSifInitIopHeap` | 1 | IOP heap and lifetime; F2 |
| `0x00335B50` | `hle_sceSifAllocIopHeap` | 3 | IOP heap and lifetime; F2 |
| `0x00335BC0` | `hle_sceSifAllocSysMemory` | 0 | IOP heap and lifetime; F2 |
| `0x00335C40` | `hle_sceSifFreeSysMemory` | 1 | IOP heap and lifetime; F2 |
| `0x00335CB8` | `hle_sceSifFreeIopHeap` | 1 | IOP heap and lifetime; F2 |
| `0x00335DC8` | `hle_sceSifQueryMemSize` | 1 | IOP heap and lifetime; F2 |
| `0x00335E38` | `hle_sceSifQueryMaxFreeMemSize` | 1 | IOP heap and lifetime; F2 |
| `0x00335EA8` | `hle_sceSifQueryTotalFreeMemSize` | 1 | IOP heap and lifetime; F2 |
| `0x003361A8` | `hle_sceSifLoadFileReset` | 1 | Module/reset stub contract |
| `0x003363E8` | `hle_sceSifStopModule` | 0 | Module/reset stub contract |
| `0x003365F0` | `hle_sceSifUnloadModule` | 0 | Module/reset stub contract |
| `0x00336680` | `hle_sceSifSearchModuleByName` | 1 | Module/reset stub contract |
| `0x00336720` | `hle_sceSifSearchModuleByAddress` | 0 | Module/reset stub contract |
| `0x003367B0` | `hle_sceSifLoadModuleBuffer` | 0 | Module/reset stub contract |
| `0x003367D0` | `hle_sceSifLoadStartModuleBuffer` | 0 | Module/reset stub contract |
| `0x00336A18` | `hle_sceSifLoadModule` | 19 | Module/reset stub contract |
| `0x00336A38` | `hle_sceSifLoadStartModule` | 0 | Module/reset stub contract |
| `0x00336B60` | `hle_sceSifLoadElfPart` | 0 | Module/reset stub contract |
| `0x00336B80` | `hle_sceSifLoadElf` | 0 | Module/reset stub contract |
| `0x00336D78` | `hle_sceSifResetIop` | 1 | Module/reset stub contract |
| `0x00336ED0` | `hle_sceSifIsAliveIop` | 0 | Module/reset stub contract |
| `0x00336EF8` | `hle_sceSifSyncIop` | 1 | Module/reset stub contract |
| `0x00336F48` | `hle_sceSifRebootIop` | 1 | Module/reset stub contract |
| `0x003396C0` | `hle_sceDeci2Open` | 1 | Debug/console stub contract |
| `0x00339708` | `hle_sceDeci2Close` | 0 | Debug/console stub contract |
| `0x00339730` | `hle_sceDeci2ReqSend` | 1 | Debug/console stub contract |
| `0x00339760` | `hle_sceDeci2Poll` | 1 | Debug/console stub contract |
| `0x003397E0` | `hle_sceDeci2ExRecv` | 1 | Debug/console stub contract |
| `0x00339818` | `hle_sceDeci2ExSend` | 1 | Debug/console stub contract |
| `0x003398D0` | `hle_kputs` | 2 | Debug/console stub contract |
| `0x00339970` | `hle_sceSifInitCmd` | 1 | SIF/RPC layout, completion and bypasses |
| `0x00339BF0` | `hle_sceSifExitCmd` | 1 | SIF/RPC layout, completion and bypasses |
| `0x0033A020` | `hle_sceSifWriteBackDCache` | 37 | SIF/RPC layout, completion and bypasses |
| `0x0033B880` | `hle_scePad2Init` | 1 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033B8C0` | `hle_scePad2End` | 1 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033B928` | `hle_scePad2CreateSocket` | 1 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033BA30` | `hle_scePad2DeleteSocket` | 2 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033BA88` | `hle_scePad2Read` | 1 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033BB60` | `hle_scePad2GetButtonProfile` | 1 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033BC30` | `hle_scePad2GetState` | 2 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033BE50` | `hle_scePad2InitDmaDBuff` | 2 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033BEC8` | `hle_scePad2LinkDriver` | 4 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033BF20` | `hle_scePad2GetSide` | 2 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033BF88` | `hle_scePad2GetSide2` | 2 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033BFF0` | `hle_scePad2CheckDma` | 1 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033C050` | `hle_scePad2SetButtonOrder` | 2 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033C138` | `hle_sceVibGetProfile` | 1 | Pad layout, state and lifecycle; F3/F4 |
| `0x0033DAA8` | `hle_sceCdCallback` | 0 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033DC80` | `hle_sceCdInitEeCB` | 0 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033DF60` | `hle_sceCdPOffCallback` | 0 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033E0B0` | `hle_sceCdLayerSearchFile` | 1 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033E3C8` | `hle_sceCdSearchFile` | 4 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033E558` | `hle_sceCdDiskReady` | 3 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033E5F0` | `hle_sceCdSync` | 5 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033E690` | `hle_sceCdSyncS` | 3 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033E920` | `hle_sceCdInit` | 3 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033EDF8` | `hle_sceCdDiskReady` | 3 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033F000` | `hle_sceCdRead` | 2 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033F1E0` | `hle_sceCdReadIOPm` | 1 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033F338` | `hle_sceCdGetToc` | 1 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033F5C8` | `hle_sceCdSeek` | 1 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033F730` | `hle_sceCdGetError` | 5 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033F7C8` | `hle_sceCdStatus` | 1 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033F880` | `hle_sceCdBreak` | 2 | Disc API, buffers and completion; F1 / TOC gap |
| `0x0033F938` | `hle_sceCdMmode` | 2 | Disc API, buffers and completion; F1 / TOC gap |
