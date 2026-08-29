# Viewport fitting

Dreamcast pages are displayed at 640×480 and POPSurf does not currently offer
horizontal scrolling. Modern pages sometimes declare a minimum or fixed width
larger than the screen, making content unreachable.

When viewport fitting is enabled, POPSurf clamps an element's `width` and
`min-width` to its containing block. This is an intentional CSS deviation. It
does not alter colors, fonts, margins, or line breaking, and it can be disabled
at runtime with `litehtml::ps_fit_to_viewport`.

The comparison harness in `tests/layout-host/` renders both modes:

```sh
make -C tests/layout-host
tests/layout-host/layouttest --compare page.html
```

Pages that already fit should have identical geometry. Pages wider than the
viewport should report a smaller fitted edge.

POPSurf also carries a separate table-cell height correction in
`vendor/litehtml/src/render_item.cpp`. That is an upstream bug fix; viewport
fitting is a project-specific policy.
