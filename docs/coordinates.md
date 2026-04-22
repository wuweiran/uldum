# Uldum Engine — Coordinate System Design

## Game Coordinates (used everywhere in engine code)

```
     Z (up/height)
     |
     |  Y (forward/north)
     | /
     |/
     +-------> X (right/east)
```

- **X** = right / east
- **Y** = forward / north
- **Z** = up / height (ground level = 0)
- **Facing** = rotation around Z axis, in radians. 0 = facing +Y (forward).
- **Right-handed** coordinate system.
- **World origin (0, 0) = map center** (WC3 convention). A 32×32 tile map at 128 units/tile extends (-2048..+2048) on each axis. The south-west corner is at `(origin_x(), origin_y())` = `(-world_width/2, -world_height/2)`.

## Where Game Coordinates Are Used

| Layer | Details |
|-------|---------|
| Simulation (`Transform`) | `position` is (X, Y, Z) in game coords. `facing` is rotation around Z. |
| Unit creation | `create_unit(world, type, owner, x, y)` places on XY ground plane, Z = terrain height. |
| Camera | Position in game coords. Movement on XY plane (WASD), height on Z (Q/E). Up vector = (0,0,1). Yaw 0 = looking toward +Y. |
| Terrain data | Heightmap indexed by grid (ix, iy). World position: `x = td.vertex_world_x(ix)`, `y = td.vertex_world_y(iy)`, `z = td.world_z_at(ix, iy)`. Grid (0, 0) is the SW vertex and sits at world `(origin_x(), origin_y())`. |
| Terrain mesh | Vertices in game coords. Normals computed via `cross(dx, dy)` in game space. |
| Shaders (world space) | Vertex shader outputs `frag_world_normal = mat3(model) * in_normal` in game coords. Fragment shader light direction is in game coords. |

## External Coordinate Conventions

### Vulkan Clip Space

```
     +-------> X (right)
     |
     |
     v
     Y (down)

     Z = depth (0 = near, 1 = far)
```

- Y points **down** (opposite of OpenGL).
- Depth range [0, 1] (not [-1, 1] like OpenGL).
- **Conversion**: the camera projection matrix flips Y: `m_proj[1][1] *= -1.0f`. This is the **only** place Vulkan's clip convention is handled. All other code uses game coordinates.

### glTF Models (Y-up)

```
     Y (up)
     |
     |  Z (backward / toward viewer)
     | /
     |/
     +-------> X (right)
```

- glTF uses Y-up, Z-backward.
- **Conversion**: the renderer applies a -90 degree rotation around X in the model matrix to convert Y-up to Z-up. This only applies to loaded glTF meshes, not engine-generated geometry.
- The `GpuMesh::native_z_up` flag tracks whether a mesh is already in game coordinates (skip rotation) or needs the glTF conversion.

### GLM

- GLM is coordinate-agnostic. `glm::lookAt`, `glm::perspective`, etc. work with whatever coordinate system you give them.
- We pass Z-up game coordinates to `glm::lookAt` with `up = (0, 0, 1)`. This works correctly.
- `glm::perspective` produces an OpenGL-style projection (Y-up, depth [-1,1]). The Y-flip in the projection matrix converts this to Vulkan conventions.

## Winding Convention

- Pipeline front face: `VK_FRONT_FACE_COUNTER_CLOCKWISE`.
- For a visible face, `cross(edge1, edge2)` of triangle vertices must equal the **outward face normal**.
- The Vulkan Y-flip in the projection matrix reverses apparent screen-space winding, but `glm::lookAt` with Z-up and the Y-flip together produce consistent results: geometric outward normals = front-facing.
- Terrain and all engine-generated meshes follow this convention.

## Rules

1. **All game code uses game coordinates.** No exceptions.
2. **Vulkan conversion happens only in the projection matrix** (Y-flip). Do not scatter Vulkan-specific coordinate logic elsewhere.
3. **glTF conversion happens only in the model matrix** (renderer draw loop). The `native_z_up` flag on `GpuMesh` controls whether the rotation is applied.
4. **Shaders receive world-space normals** in game coordinates. Light directions, view directions, etc. in shaders are all in game coordinates.
5. **Never convert to Y-up** in engine code. If a library or format uses Y-up, convert at the boundary and keep everything internal in Z-up game coords.
