# Miami 3.1.6 technical audit

Source under analysis (not part of this repository):
`../../../gameloft/miami/decompiled_miami/lib/armeabi/libGangster2.so`.

## Binary identity

| Property | Value |
|---|---|
| File size | 9,793,266 bytes |
| SHA-256 | `ad52892fa12bcf00574e1d0f51df7642802ca648c2bdc2f79372f9aa478c5b21` |
| ELF | 32-bit little-endian ARM shared object, EABI5 |
| CPU attributes | ARMv7-A, Thumb-1, VFPv3 |
| ABI | softfp (no `Tag_ABI_VFP_args`) |
| Stripping | not stripped; large symbol/string tables remain |
| Unique undefined imports | 312 |
| JNI exports | 24 |

The file contains symbol information, but calling it “full debug info” would be
misleading: the debug sections are small relative to the symbol/string tables.

## Dynamic shape

- Needed libraries: `libc.so`, `libGLESv2.so`, `libGLESv1_CM.so`,
  `libstdc++.so`, `libm.so`, `libdl.so`, `liblog.so`.
- Init array: 1,804 bytes, or 451 four-byte constructor entries.
- Dynamic relocations: 376,728 bytes / 8 = 47,091 entries.
- PLT relocations: 2,424 bytes / 8 = 303 entries.
- Relative relocation fast-path count: 43,577.

This constructor/relocation load is materially larger than West Coast Hustle,
so the loader must log around relocation and initialization separately.

## Baseline choice

Direct resolver-table overlap before Miami-specific additions:

| Candidate | Covered Miami imports |
|---|---:|
| 9mm Vita | 292 / 312 |
| MC2 Vita | 289 / 312 |

9mm was selected for its broader import and GLES compatibility. The boot order
was not copied blindly: it was reconstructed from Miami's smali. The older
Gangstar West Coast Hustle binary was used to confirm the engine-family shape,
but its available artifact is not a maintainable source baseline.

## Assets

The cache is already loose (`Gangstar2/`, 3,217 files), and the native library
hardcodes `/sdcard/gameloft/games/Gangstar2/`. The port therefore maps directly
to loose files. There is no OBB extraction layer in this design.
