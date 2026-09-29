# Kiln: the kpak container format (v2)

`.kpak` is the per-target asset container written by the `pack_target` rule.
Index sidecar: `<target>.kpak.json`.

## Layout (v2)

All integers little-endian. The magic is the version: "KPAK2" marks v2
("KPAK1" was the v1 layout: 48-byte entries with 32-hex ids, no header).

```
offset 0     magic: "KPAK2"                        (5 bytes)
offset 5     u32 table_count
offset 9     u32 table_offset                      (bytes from file start; fixed at 45)
offset 13    u8[32] build_hash                     (raw BLAKE3 of the table bytes)
offset 45    table[table_count]:                   (36 bytes each)
                 bytes[16]  asset id (the raw 128-bit AssetID: High u64 LE,
                            Low u64 LE - the canonical AssetID::ToString form
                            is hex(High)+hex(Low))
                 u32        asset type (the cooked blob's TinyAssetType; 0 if
                            the blob isn't a .tinyasset)
                 u64        blob offset (from file start)
                 u64        blob size
then         blobs (raw, in id-sorted order)
```

Entries are sorted by asset id ascending (High then Low). Blobs are stored
uncompressed; the format is versioned by magic so a future v3 may add zstd
framing without breaking readers. `build_hash = BLAKE3(serialized table
bytes + each blob's 64-hex content hash from the provider manifest, in
table order)`: two builds of identical content + cook settings produce the
same hash; ANY asset change changes it (the content hashes catch same-size
edits the table alone would miss). Peers exchange `build_hash` at
handshake; assets are referenced over the wire by their table
position (`AssetIndex`, u32).

## Index JSON (format 1, unchanged)

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
Offsets are the v2 layout's blob positions; the index stays the runtime's
id→path source (the table carries no paths).

## Readers

Read the magic; "KPAK2" → parse the header + table (the runtime's
`KpakArchive::IndexOf`/`IdAt`/`TypeAt`/`BuildHashHex`). Blob reads use the
index JSON's offset/size (unchanged path). No decompression in v2. The
runtime should load the index once at startup and read blobs lazily.
