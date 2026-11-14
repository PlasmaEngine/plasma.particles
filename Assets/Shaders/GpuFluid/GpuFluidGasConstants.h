#pragma once

#include <Shaders/GpuFluid/GpuFluidConstants.h>

// Kernel modes (GpuFluidGasPass.x).
#define GPU_FLUID_HALO_SCALAR 0          // scalars: neighbour interiors, 0 in open air and beyond open faces, zero gradient at closed faces
#define GPU_FLUID_HALO_VELOCITY 1        // MAC velocity: neighbour interiors, ambient in open air, ambient inflow or zero gradient beyond open faces, no flow through closed ones
#define GPU_FLUID_HALO_VELOCITY_KEEP 2   // after the projection: neighbour interiors only, boundary faces keep what the projection wrote
#define GPU_FLUID_HALO_COPY 3            // any field: neighbour interiors, zero gradient everywhere beyond the domain, 0 in open air (the curl)

#define GPU_FLUID_EMITTER_SPHERE 0
#define GPU_FLUID_EMITTER_BOX 1

// GasAdvect modes.
#define GPU_FLUID_ADVECT_SEMI_LAGRANGIAN 0
#define GPU_FLUID_ADVECT_REVERSE 1          // scalars: MacCormack reverse trace into Scratch
#define GPU_FLUID_ADVECT_CORRECT 2          // scalars: MacCormack limited correction
#define GPU_FLUID_ADVECT_VELOCITY_REVERSE 3 // velocity: MacCormack reverse trace of every MAC component into Scratch.xyz
#define GPU_FLUID_ADVECT_VELOCITY_CORRECT 4 // velocity: MacCormack limited correction, each component clamped to its 8 source texels

// Pressure solvers (GpuFluid.Gas.PressureSolver).
#define GPU_FLUID_PRESSURE_RBSOR 0
#define GPU_FLUID_PRESSURE_MULTIGRID 1

// Face codes of the pressure operator, 2 bits per face of a cell, face f = axis * 2 + (side > 0), in GpuFluidFaceCodes
// (one uint per interior cell). Bit GPU_FLUID_CODE_SOLID_BIT marks a cell inside a solid: it takes no part at all.
#define GPU_FLUID_CODE_WALL 0      // closed domain face: Neumann, the face carries no flow
#define GPU_FLUID_CODE_FLUID 1     // the neighbour is a fluid cell: coupled
#define GPU_FLUID_CODE_DIRICHLET 2 // open air beyond (unallocated brick, open domain face): Phi = 0 there
#define GPU_FLUID_CODE_SOLID 3     // the face touches a solid: Neumann, the face keeps the solid's velocity
#define GPU_FLUID_CODE_SOLID_BIT 12

// Multigrid of the gas pressure (GasMGSetup, GasMGFine, GasMGMid, GasMGDense). Levels: F (8 cubed per brick, the
// pressure buffer), M (4 cubed per brick), C (2 cubed per brick), then the dense brick grid D0 (one cell per virtual
// brick) halved down to at most GPU_FLUID_MG_COARSEST_CELLS cells.
//
// Per pool slot, GpuFluidMGLocal holds GPU_FLUID_MG_LOCAL_STRIDE floats: x of M (two sides), b of M, x of C (two sides),
// b of C. GpuFluidMGCoeff holds GPU_FLUID_MG_COEFF_STRIDE float4: two per M cell, then two per C cell, each pair
// (w -x, w +x, w -y, w +y) (w -z, w +z, s, diag): face couplings, the Dirichlet sink and their sum. The dense levels
// keep x and b in GpuFluidMGDense (float2 per cell) and the same coefficient pair in GpuFluidMGDenseCoeff, where a cell
// of open air (no allocated brick under it) has diag = -1.
#define GPU_FLUID_MG_XM0 0
#define GPU_FLUID_MG_XM1 64
#define GPU_FLUID_MG_BM 128
#define GPU_FLUID_MG_XC0 192
#define GPU_FLUID_MG_XC1 200
#define GPU_FLUID_MG_BC 208
#define GPU_FLUID_MG_LOCAL_STRIDE 216
#define GPU_FLUID_MG_COEFF_C 128 // first float4 of the C level
#define GPU_FLUID_MG_COEFF_STRIDE 144
// GpuFluidMGMask: 4 uints per slot, which cells of M (words 0, 1: 64 bits) and of C (word 2: 8 bits) take part
// (diag > 0), so a prolongation finds its coarse samples' kinds without reading their coefficients.
#define GPU_FLUID_MG_MASK_STRIDE 4
#define GPU_FLUID_MG_MAX_DENSE_LEVELS 12
#define GPU_FLUID_MG_COARSEST_CELLS 8

// GasMGFine modes (one 8 cubed group per brick). A V-cycle's launches, block red-black (brick colours a, b):
// GasMGFine SMOOTH_RESTRICT(a, b of M against b's old Phi), SMOOTH_RESTRICT(b), GasMGMid FIX_RESTRICT(a),
// MID_RESTRICT_COARSE(b), COARSE_RESTRICT(a), dense, COARSE_PROLONG(a, b), MID_PROLONG(b, a), GasMGFine PROLONG(a, b).
#define GPU_FLUID_MG_FINE_SMOOTH 0          // F pre-smoothing, Phi in -> Phi out
#define GPU_FLUID_MG_FINE_RESTRICT 1        // F residual -> b of M, M pre-smoothing from 0
#define GPU_FLUID_MG_FINE_PROLONG 2         // F += P(x of M), post-smoothing, Phi in -> Phi out
#define GPU_FLUID_MG_FINE_SMOOTH_RESTRICT 3 // FINE_SMOOTH, then FINE_RESTRICT on the smoothed tile
#define GPU_FLUID_MG_FINE_RESTRICT_MID 4    // FINE_RESTRICT, then the M residual -> b of C and C pre-smoothing from 0
// GasMGMid modes (one 64 thread group per brick).
#define GPU_FLUID_MG_MID_RESTRICT 0        // M residual -> b of C, C pre-smoothing from 0
#define GPU_FLUID_MG_COARSE_RESTRICT 1     // C residual -> b of D0, x of D0 = 0
#define GPU_FLUID_MG_COARSE_PROLONG 2      // C += P(x of D0), post-smoothing
#define GPU_FLUID_MG_MID_PROLONG 3         // M += P(x of C), post-smoothing
#define GPU_FLUID_MG_MID_RESTRICT_COARSE 4 // MID_RESTRICT, then COARSE_RESTRICT on the smoothed C tile
#define GPU_FLUID_MG_MID_FIX_RESTRICT 5    // b of M from SMOOTH_RESTRICT's partial one and the other colour's change, M
                                           // pre-smoothing, M residual -> b of C, C pre-smoothing from 0
// GasMGDense modes.
#define GPU_FLUID_MG_DENSE_SETUP 0    // one thread per cell of level Params0.w: open-air cells of D0, coefficients of the coarser levels
#define GPU_FLUID_MG_DENSE_SMOOTH 1   // one thread per cell of level Params0.w, one colour (Pass.y)
#define GPU_FLUID_MG_DENSE_RESTRICT 2 // one thread per cell of level Params0.w + 1: residual of the level above, restricted
#define GPU_FLUID_MG_DENSE_PROLONG 3  // one thread per cell of level Params0.w: += P(x of the level below), no smoothing
#define GPU_FLUID_MG_DENSE_CYCLE 4    // one group: the whole V-cycle from level Params0.w down and back
#define GPU_FLUID_MG_DENSE_SETUP_TAIL 5 // one group: coefficients from level Params0.w + 1 down to the coarsest

// GasPoisson modes (N3).
#define GPU_FLUID_POISSON_SETUP 0    // the manufactured right-hand side into the divergence buffer, Phi = 0
#define GPU_FLUID_POISSON_RESIDUAL 1 // max |b - A Phi| into stats word Params0.x
#define GPU_FLUID_POISSON_ERROR 2    // max |Phi - Phi*| into stats word Params0.x

// Solids: at most this many shapes per volume and tick (GasSolids).
#define GPU_FLUID_MAX_SOLID_SHAPES 64
#define GPU_FLUID_SOLID_FLAG_WORDS 16 // bits of the 512 interior cells of a brick
#define GPU_FLUID_SHAPE_SPHERE 0
#define GPU_FLUID_SHAPE_BOX 1
#define GPU_FLUID_SHAPE_CAPSULE 2
#define GPU_FLUID_SHAPE_PLANE 3

/// One analytic solid of a gas tick, in voxels of the volume's grid (cell (i,j,k) spans [i, i+1)), velocities in m/s in
/// the volume's space.
struct PL_SHADER_STRUCT plGpuFluidSolidShape
{
  FLOAT4(CenterType);      ///< xyz: centre (voxels), w: GPU_FLUID_SHAPE_*
  FLOAT4(Axis0);           ///< xyz: box local x / capsule axis / plane normal (unit), w: box half extent x, capsule half height, sphere radius (voxels)
  FLOAT4(Axis1);           ///< xyz: box local y, w: box half extent y, capsule radius (voxels)
  FLOAT4(Axis2);           ///< xyz: box local z, w: box half extent z
  FLOAT4(LinearVelocity);  ///< xyz: m/s at the centre
  FLOAT4(AngularVelocity); ///< xyz: rad/s about the centre
  FLOAT4(BoundsMin);       ///< xyz: AABB in voxels
  FLOAT4(BoundsMax);
};

/// Per dispatch data of every gas solver kernel. Push constants, because render-graph passes are recorded before they
/// execute and a shared constant buffer would be last-write-wins. 128 bytes, the Metal limit.
///
/// Units: positions in voxels of the virtual grid (cell (i,j,k) spans [i, i+1)), velocity in m/s, time in seconds.
/// Temperature is normalised to [0, 1] of the preset's maximum.
BEGIN_PUSH_CONSTANTS(plGpuFluidGasConstants)
{
  UINT4(GpuFluidGasGrid);    // xyz: bricks of the virtual grid, w: pool capacity in bricks
  UINT4(GpuFluidGasAtlas);   // xyz: atlas bricks per axis, w: open-face mask (GPU_FLUID_FACE_*)
  UINT4(GpuFluidGasPass);    // x: mode, y: red-black colour or emitter shape, z: 1 on the frame's last tick, w: kernel flags
  FLOAT4(GpuFluidGasStep);   // x: dt, y: voxel size in metres, z: 1 / voxel size, w: simulation time
  FLOAT4(GpuFluidGasParams0); // kernel specific, see the kernel
  FLOAT4(GpuFluidGasParams1);
  FLOAT4(GpuFluidGasParams2);
  FLOAT4(GpuFluidGasParams3);
}
END_PUSH_CONSTANTS(plGpuFluidGasConstants)

// What the kernels put in Params0..3:
//
// GasEmit:     0: centre xyz (voxels), radius (voxels, sphere)     1: box half extents xyz (voxels), edge softness (voxels)
//              2: density rate (1/s), temperature, fuel, noise strength (0..1)
//              3: velocity xyz (m/s), noise frequency (1/voxel)
// GasAdvect:   0: ambient wind xyz (m/s)
// GasHalo:     0: ambient wind xyz (m/s)
// GasForces:   0: buoyancy direction xyz (unit, against gravity), buoyancy per unit temperature (m/s^2)
//              1: x weight per unit density (m/s^2), y ambient temperature, z cooling rate (1/s), w density dissipation (1/s)
//              2: x density clamp, y velocity dissipation (1/s), z fuel dissipation (1/s), w edge fade width (voxels)
//              3: x edge fade rate at an open face (1/s), y vorticity confinement epsilon (0: off)
// GasSolids:   0: x shape count, y first shape of this tick
// GasPressure: 0: x over-relaxation
// GasMG*:      0: x over-relaxation, y pre-smoothing sweeps, z post-smoothing sweeps, w dense level
//              1: x coarsest-level sweeps, y multi-group dense levels above the single-group tail
// GasFinalize: 0: x allocation threshold, y brick mean floor
