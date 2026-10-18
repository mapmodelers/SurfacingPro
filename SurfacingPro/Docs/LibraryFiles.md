# SurfacingPro material files

Material instances are authored in the Material Manager. Any of them can be written out as a small JSON file - to keep, to share with another project, or to edit outside the editor - and read back in.

## Where they live

```
<Project>/SurfacingPro/Library/Materials/<Name>.material.json
```

(`<Project>/SurfacingPro` is the texture source root, set in the settings.) One file per instance.

- **Export to file** (the Material Manager's right-click) writes the shown instance.
- **Import from file...** (same menu) reads one or more files: an instance that does not exist yet is made, one that does is brought in line with its file.

## A material instance

```json
{
  "schema": "surfacingpro.material/1",
  "name": "MI_BrickWall_Old",
  "parent": "M_Opq_Simple",
  "scalars": { "Roughness": 0.85 },
  "vectors": { "Tint": [1.0, 0.95, 0.9, 1.0] },
  "switches": { "UseDetail": true },
  "textures": { "BaseColor": "/Game/SurfacingPro/Textures/T_Sp_Brick_D.T_Sp_Brick_D" },
  "surfacing": { "tile_m": [2.0, 2.0], "use": "Opaque" }
}
```

| Field | Required | Meaning |
|---|---|---|
| `schema` | no | `surfacingpro.material/1`. |
| `name` | no | The instance's asset name; the file name when absent. |
| `parent` | yes | A parent by name (looked for in the library, then among the plugin's own materials), or a full asset path (`/Game/...`). |
| `scalars`, `vectors`, `switches` | no | Parameter name → value. Colours as `[r, g, b, a]`, linear. |
| `textures` | no | Parameter name → texture asset path. |
| `surfacing` | no | `tile_m` (metres per tile, the size the material lands at), `use` (Opaque, Layer, NoDraw, Clip), `trim_cells`. |
| `asset`, `synced_at` | written by SurfacingPro | The instance, and when file and instance last agreed. Leave them alone. |

The file is the whole of the instance's overrides: a parameter the file does not name goes back to the parent's value on import. A name the parent does not have is reported and skipped.
