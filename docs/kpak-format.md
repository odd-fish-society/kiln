# The kpak container format (v2)

A `.kpak` file holds one target's assets. The `pack_target` rule writes it, along with
an index file next to it, `<target>.kpak.json`.

## Layout

All integers are little-endian. The magic doubles as the version: "KPAK2" marks this
layout. (The older "KPAK1" layout had no header and 48-byte entries with 32-character
hex ids.)

```
offset 0     magic: "KPAK2"                        (5 bytes)
offset 5     u32 table_count
offset 9     u32 table_offset                      (bytes from file start; fixed at 45)
offset 13    u8[32] build_hash                     (raw BLAKE3 of the table bytes)
offset 45    table[table_count]:                   (36 bytes each)
                 bytes[16]  asset id (the raw 128-bit id: High u64 LE,
                            Low u64 LE; its canonical hex form is
                            hex(High)+hex(Low))
                 u32        asset type (the number its cook reported: the
                            action's `asset_type`, else its dyndep's "type";
                            0 when neither says)
                 u64        blob offset (from file start)
                 u64        blob size
then         blobs (raw, in id-sorted order)
```

The table is sorted by asset id, ascending (High, then Low). Blobs are stored
uncompressed. Because the magic carries the version, a later v3 can add compression
without confusing older readers.

The build hash is BLAKE3 over the serialized table bytes followed by each blob's
64-character content hash from the provider manifest, in table order. Two builds of
the same content with the same cook settings get the same hash, and any change to any
asset changes it. The content hashes are what catch an edit that keeps a blob's size,
which the table alone would miss. A game can compare build hashes at a network
handshake, and then refer to assets over the wire by their position in the table (a
u32).

## Index JSON (format 1)

```json
{
  "format": 1,
  "assets": [
    {"id": "<64-hex>", "path": ".kiln/out/<config>/assets/<plugin>/<rel>",
     "offset": 234, "size": 12345, "tags": ["Mesh"]}
  ],
  "tags": ["Mesh", "Scene"],
  "omitted_by_tags": 119
}
```

- An asset packed as it is (an action whose `asset` is one of its own inputs) is listed
  under its committed source path, with the id its action or dyndep states.
- An entry has `tags` only when its asset has any.
- The top-level `tags` (the target's `pack.tags`, sorted) and `omitted_by_tags` (how
  many assets those tags left out) appear only when the target declares `pack.tags`.
- One id names one blob. Two assets with the same id fail the pack.
- The writer streams each blob from its file into the pack and never holds it in memory.

An asset's id is stable as long as its plugin-relative source path stays the same.
Renaming or moving the source changes the id, because a cooked asset's identity comes
from that path, not from its content.

The offsets are blob positions in the layout above. The table has no paths, so the
index is where the runtime maps an id to a path.

## Reading a pack

Read the magic. For "KPAK2", parse the header and the table, which give you id lookup,
the type numbers and the build hash. Read each blob through the offset and size in the
index JSON; there's nothing to decompress. A runtime should load the index once at
startup and read blobs when it needs them.
