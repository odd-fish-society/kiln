# Kiln: the kpak container format (v1)

`.kpak` is the per-target asset container written by the `pack_target` rule.
Index sidecar: `<target>.kpak.json`.

## Layout

All integers little-endian.

```
offset 0    magic: "KPAK1"                        (5 bytes)
offset 5    u32 count                             (entry count)
offset 9    entries[count]:                       (48 bytes each)
                bytes[32]  asset id (the FIRST 32 hex chars of the 64-hex
                           BLAKE3 of "plugin/plugin-relative path": exactly
                           the engine's 128-bit AssetID::ToString() form; the
                           full 64-hex id lives in the index/manifest, D7)
                u64        blob offset (from file start)
                u64        blob size
then        blobs (raw, in id-sorted order)
```

Entries are sorted by asset id (byte order of the hex string). Blobs are stored
uncompressed in v1; the format is versioned (`format: 1` in the index) so a
future v2 may add zstd framing without breaking readers.

## Index JSON

```json
{
  "format": 1,
  "assets": [
    {"id": "<64-hex>", "path": ".kiln/out/<config>/assets/<plugin>/<rel>",
     "offset": 234, "size": 12345}
  ]
}
```

`id` is stable across moves that keep the plugin-relative source path; a
rename changes the id (documented rename story: stable relpath, not content).

## Readers

Read the header, then seek to `offset` and read `size`. No decompression in
v1. The runtime should load the index once at startup and read blobs lazily.
