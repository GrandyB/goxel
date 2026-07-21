# Metadata templates

Metadata templates are JSON files that define a starting set of metadata items for a map.
Place `.json` files in your Goxel user folder:

- **Windows:** `%AppData%\Goxel\metadata-templates\`
- **Linux / macOS:** `~/.goxel/metadata-templates/`

On first run, Goxel copies bundled examples into that folder. After that, you can add,
edit, or remove files there freely.

Load a template from the **Metadata** panel (**Load template**). Loading replaces all
current metadata items on the open map.

Only `.json` files appear in the template picker. This readme is for reference only.

## File format

Each template file is a JSON object with a single `metadata` array. Every entry in the
array describes one item to create.

```json
{
  "metadata": [
    { "name": "MapName", "type": "Text", "default": "My map name" },
    { "name": "FlagRed", "type": "Point2D", "color": [255, 0, 0, 255], "default": [20, 0] }
  ]
}
```

## Item fields

### Required

| Field  | Type   | Description                          |
| ------ | ------ | ------------------------------------ |
| `name` | string | Display name (must not be empty).    |
| `type` | string | Item type (see below).               |

### Optional

| Field                | Applies to              | Description |
| -------------------- | ----------------------- | ----------- |
| `color`              | Points, zones, groups   | Row and map tint as `[R, G, B]` or `[R, G, B, A]` (uint8, 0–255). If omitted, a random colour is chosen. |
| `options`            | Enum                    | Array of option labels (max 32 options, 64 characters each). |
| `default`            | Text, Float, Color, Enum, Point2D, Point3D | Initial value when the template is loaded (see below). |
| `default_child_type` | Group                   | Default type for new children added with the group **+** button. Defaults to **2D Point** if omitted. |
| `lock_child_types_to_default` | Group                   | When `true`, children cannot change type in the metadata panel. Defaults to `false`. |

Unknown fields are ignored. Invalid entries are skipped with a log warning; other items
still load.

## Type strings

Type names are case-insensitive. Spaces, underscores, and hyphens are ignored, so
`Point2D`, `point2d`, and `2D Point` all work.

| Type        | Accepted examples                          | Description |
| ----------- | ------------------------------------------ | ----------- |
| **2D Point**  | `Point2D`, `2DPoint`, `2d point`          | Map marker spanning the full height of the voxel volume. |
| **3D Point**  | `Point3D`, `3DPoint`, `3d point`          | Voxel position marker. |
| **2D Zone**   | `Zone2D`, `2DZone`, `2d zone`             | Vertical zone spanning the full map height. |
| **3D Zone**   | `Zone3D`, `3DZone`, `3d zone`             | Axis-aligned box zone. |
| **Float**     | `Float`, `float`                           | Scalar number. |
| **Text**      | `Text`, `text`                             | Short text string. |
| **Color**     | `Color`, `Colour`, `color`                | Colour value (RGBA in the metadata panel). |
| **Enum**      | `Enum`, `enum`                             | Fixed list of choices (requires `options`). |
| **Group**     | `Group`, `group`                           | Container for multiple items of the same spatial type. |

## Default values (`default`)

| Type     | JSON form | Example |
| -------- | --------- | ------- |
| Text     | string    | `"default": "My map name"` |
| Float    | number    | `"default": 100.0` |
| Color    | uint8 array | `"default": [127, 127, 151]` or `[R, G, B, A]` |
| Enum     | option label or index | `"default": "Water"` or `"default": 0` |
| Point2D  | `[x, y]`  | `"default": [20, 0]` — Z is set to the map floor. |
| Point3D  | `[x, y, z]` | `"default": [10, 20, 5]` |
| 2D/3D Zone | —       | Not supported in templates; zones start at the map centre with a small default size. |

For **Enum**, the label must match an entry in `options` exactly. An integer index selects
the option by position (0-based).

## Groups

Templates use a **flat** list. A group entry only creates the empty group; it does not
define child items in JSON. After loading, use the group **+** button to add children.
The `default_child_type` field sets which type those children use by default.
Set `lock_child_types_to_default` to `true` to prevent changing child types in the editor.

Child items added in the editor inherit their parent group's colour unless they define
their own `color`.

## Example

See `trenchblocks.json` in this folder for a game-style template with text fields,
colours, enums, spawn points, and pickup groups.
