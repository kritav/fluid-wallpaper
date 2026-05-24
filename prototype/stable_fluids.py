"""
Stable Fluids — 2D CPU reference prototype.

Implements Jos Stam, "Real-Time Fluid Dynamics for Games" (GDC 2003):
    https://www.dgp.toronto.edu/people/stam/reality/Research/pdf/GDC03.pdf

The structure (add_source / diffuse / project / advect / set_bnd) mirrors
Stam's pseudocode 1:1 so the future CUDA port can be diffed against this
implementation when something looks wrong on the GPU.

One deliberate deviation: the linear solver uses Jacobi iteration rather
than Gauss-Seidel. NumPy can express a Jacobi sweep as one slice
assignment; a true Gauss-Seidel sweep needs a Python for-loop over each
cell, which is unusably slow at N=128. Both schemes converge to the same
Poisson / diffusion solution given enough iterations, and the GPU port
will use Jacobi anyway (Gauss-Seidel without red-black coloring is
sequential and doesn't parallelize).

Grid layout (matches Stam): arrays are (N+2, N+2) float32, where indices
0 and N+1 are ghost cells holding boundary values and 1..N is the
interior. The first axis is x and the second is y; this aligns with
pygame.surfarray (which expects (W, H, 3)) and pygame's mouse (x, y),
so what you see on screen matches the array layout.

Controls:
    left-drag       inject dye + push velocity in drag direction
    right-click     clear the simulation
    Esc / close     quit
"""

import sys
import numpy as np
import pygame

# --- simulation parameters --------------------------------------------------
N = 128                  # interior grid resolution
CELL_PIXELS = 5          # on-screen size of each cell
WINDOW = N * CELL_PIXELS
SOLVER_ITERS = 20        # Jacobi iterations per linear solve
DT = 0.1
VISC = 0.0               # kinematic viscosity (0 = no velocity diffusion)
DIFF = 0.0               # dye diffusion (0 = sharp dye edges)
FORCE_SCALE = 4.0        # pixels-of-drag → velocity magnitude
DYE_AMOUNT = 200.0       # dye added per frame while left button is held
SPLAT_RADIUS = 2         # cells; impulse is painted into a (2r+1)^2 block

SHAPE = (N + 2, N + 2)


# --- boundary conditions ----------------------------------------------------
def set_bnd(b, x):
    """Reflect/copy ghost cells.

    b == 0: scalar field (copy)         — used for density, divergence, pressure
    b == 1: x-component of velocity     — flip across left/right walls
    b == 2: y-component of velocity     — flip across top/bottom walls
    """
    # left / right walls (x = 0 and x = N+1)
    if b == 1:
        x[0,  1:-1] = -x[1,  1:-1]
        x[-1, 1:-1] = -x[-2, 1:-1]
    else:
        x[0,  1:-1] =  x[1,  1:-1]
        x[-1, 1:-1] =  x[-2, 1:-1]

    # top / bottom walls (y = 0 and y = N+1)
    if b == 2:
        x[1:-1,  0] = -x[1:-1,  1]
        x[1:-1, -1] = -x[1:-1, -2]
    else:
        x[1:-1,  0] =  x[1:-1,  1]
        x[1:-1, -1] =  x[1:-1, -2]

    # corners — average of the two adjacent edge cells
    x[ 0,  0] = 0.5 * (x[ 1,  0] + x[ 0,  1])
    x[ 0, -1] = 0.5 * (x[ 1, -1] + x[ 0, -2])
    x[-1,  0] = 0.5 * (x[-2,  0] + x[-1,  1])
    x[-1, -1] = 0.5 * (x[-2, -1] + x[-1, -2])


# --- linear solver (Jacobi; see file header for why not Gauss-Seidel) -------
def lin_solve(b, x, x0, a, c):
    """Solve (I - a·Laplacian) x = x0 by `SOLVER_ITERS` Jacobi sweeps.

    For each interior cell: x[i,j] = (x0[i,j] + a*sum_of_4_neighbors) / c.
    NumPy evaluates the entire RHS into a temporary, then assigns — so
    the four neighbor reads see the old x, which is exactly Jacobi.
    """
    for _ in range(SOLVER_ITERS):
        x[1:-1, 1:-1] = (
            x0[1:-1, 1:-1]
            + a * (x[0:-2, 1:-1] + x[2:, 1:-1]
                 + x[1:-1, 0:-2] + x[1:-1, 2:])
        ) / c
        set_bnd(b, x)


def diffuse(b, x, x0, diff, dt):
    a = dt * diff * N * N
    lin_solve(b, x, x0, a, 1.0 + 4.0 * a)


# --- semi-Lagrangian advection ----------------------------------------------
# Precompute interior cell index grids once; advect runs every frame.
_I = np.arange(1, N + 1, dtype=np.float32).reshape(-1, 1)
_J = np.arange(1, N + 1, dtype=np.float32).reshape(1, -1)


def advect(b, d, d0, u, v, dt):
    """For each cell, trace velocity backward one dt, sample d0 there (bilinear)."""
    dt0 = dt * N

    x = _I - dt0 * u[1:-1, 1:-1]
    y = _J - dt0 * v[1:-1, 1:-1]
    np.clip(x, 0.5, N + 0.5, out=x)
    np.clip(y, 0.5, N + 0.5, out=y)

    i0 = np.floor(x).astype(np.int32); i1 = i0 + 1
    j0 = np.floor(y).astype(np.int32); j1 = j0 + 1
    s1 = x - i0; s0 = 1.0 - s1
    t1 = y - j0; t0 = 1.0 - t1

    d[1:-1, 1:-1] = (
        s0 * (t0 * d0[i0, j0] + t1 * d0[i0, j1]) +
        s1 * (t0 * d0[i1, j0] + t1 * d0[i1, j1])
    )
    set_bnd(b, d)


# --- pressure projection (make velocity divergence-free) --------------------
def project(u, v, p, div):
    h = 1.0 / N
    div[1:-1, 1:-1] = -0.5 * h * (
        u[2:, 1:-1] - u[0:-2, 1:-1] +
        v[1:-1, 2:] - v[1:-1, 0:-2]
    )
    p.fill(0.0)
    set_bnd(0, div)
    set_bnd(0, p)

    lin_solve(0, p, div, 1.0, 4.0)

    u[1:-1, 1:-1] -= 0.5 * (p[2:, 1:-1] - p[0:-2, 1:-1]) / h
    v[1:-1, 1:-1] -= 0.5 * (p[1:-1, 2:] - p[1:-1, 0:-2]) / h
    set_bnd(1, u)
    set_bnd(2, v)


# --- one full velocity / density step ---------------------------------------
def vel_step(u, v, u_src, v_src, p, div, visc, dt):
    # add_source
    u += dt * u_src
    v += dt * v_src

    # diffuse — solve into u/v, reading from a frozen copy
    u0 = u.copy(); diffuse(1, u, u0, visc, dt)
    v0 = v.copy(); diffuse(2, v, v0, visc, dt)

    project(u, v, p, div)

    # advect each velocity component along the current velocity field
    u0 = u.copy(); v0 = v.copy()
    advect(1, u, u0, u0, v0, dt)
    advect(2, v, v0, u0, v0, dt)

    project(u, v, p, div)


def dens_step(d, d_src, u, v, diff, dt):
    d += dt * d_src
    d0 = d.copy(); diffuse(0, d, d0, diff, dt)
    d0 = d.copy(); advect(0, d, d0, u, v, dt)


# --- input → fields ---------------------------------------------------------
def splat(field, gi, gj, value):
    """Add `value` into a (2*SPLAT_RADIUS+1)^2 block centered at (gi, gj)."""
    r = SPLAT_RADIUS
    i0 = max(1, gi - r); i1 = min(N + 1, gi + r + 1)
    j0 = max(1, gj - r); j1 = min(N + 1, gj + r + 1)
    field[i0:i1, j0:j1] += value


def pixel_to_cell(mx, my):
    """Map pygame mouse pixels → interior grid indices in [1, N]."""
    gi = int(mx / CELL_PIXELS) + 1
    gj = int(my / CELL_PIXELS) + 1
    return max(1, min(N, gi)), max(1, min(N, gj))


# --- main loop --------------------------------------------------------------
def main():
    pygame.init()
    screen = pygame.display.set_mode((WINDOW, WINDOW))
    pygame.display.set_caption("Stable Fluids")
    clock = pygame.time.Clock()

    u   = np.zeros(SHAPE, dtype=np.float32)
    v   = np.zeros(SHAPE, dtype=np.float32)
    d   = np.zeros(SHAPE, dtype=np.float32)
    # scratch buffers reused by project() so we don't realloc per frame
    p   = np.zeros(SHAPE, dtype=np.float32)
    div = np.zeros(SHAPE, dtype=np.float32)
    # per-frame source buffers
    u_src = np.zeros(SHAPE, dtype=np.float32)
    v_src = np.zeros(SHAPE, dtype=np.float32)
    d_src = np.zeros(SHAPE, dtype=np.float32)

    prev_mouse = None
    running = True
    while running:
        u_src.fill(0.0); v_src.fill(0.0); d_src.fill(0.0)

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN and event.key == pygame.K_ESCAPE:
                running = False
            elif event.type == pygame.MOUSEBUTTONDOWN and event.button == 3:
                u.fill(0.0); v.fill(0.0); d.fill(0.0)

        mx, my = pygame.mouse.get_pos()
        buttons = pygame.mouse.get_pressed()
        if buttons[0]:
            gi, gj = pixel_to_cell(mx, my)
            splat(d_src, gi, gj, DYE_AMOUNT)
            if prev_mouse is not None:
                dx = (mx - prev_mouse[0]) * FORCE_SCALE
                dy = (my - prev_mouse[1]) * FORCE_SCALE
                splat(u_src, gi, gj, dx)
                splat(v_src, gi, gj, dy)
            prev_mouse = (mx, my)
        else:
            prev_mouse = None

        vel_step(u, v, u_src, v_src, p, div, VISC, DT)
        dens_step(d, d_src, u, v, DIFF, DT)

        # render interior density as grayscale, scaled up to the window size
        interior = d[1:-1, 1:-1]
        gray = np.clip(interior, 0.0, 255.0).astype(np.uint8)
        rgb = np.repeat(gray[:, :, None], 3, axis=2)
        surf_small = pygame.surfarray.make_surface(rgb)
        surf = pygame.transform.scale(surf_small, (WINDOW, WINDOW))
        screen.blit(surf, (0, 0))
        pygame.display.flip()

        clock.tick()
        pygame.display.set_caption(f"Stable Fluids — {clock.get_fps():.1f} FPS")

    pygame.quit()
    sys.exit(0)


if __name__ == "__main__":
    main()
