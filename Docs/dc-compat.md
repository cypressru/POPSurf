# Dreamcast compatibility

POPSurf targets web content from the Dreamcast era. This page describes the
current implementation, not a roadmap.

## Implemented

- HTML and CSS layout through litehtml and Gumbo
- Tables, framesets, frames, image maps, marquees, and blinking text
- GIF animation, JPEG, PNG, and tiled backgrounds
- Links, history, bookmarks, free-pointer navigation, and an on-screen keyboard
- Dreamcast controller, keyboard, and mouse input
- Text, password, checkbox, radio, select, textarea, button, and file controls
- URL-encoded form submission with GET or POST
- HTTP/1.1, DNS, redirects, chunked responses, keep-alive, and byte ranges
- MIDI, ADX, and SWF audio
- A SWF 4 player with an ActionScript 1 interpreter
- A Java applet runtime targeting part of the JDK 1.1 API

## Limitations

- HTTP only; HTTPS requires an external HTTP proxy
- Broadband Adapter or local `/pc/` files only; modem/PPP is not implemented
- No cookies
- No multipart form upload; file controls are visual only
- No JavaScript runtime
- SWF and Java support are compatibility subsets, not complete implementations
- One live page at a time and fixed memory limits suitable for a 16 MB console

## User agent

```text
Mozilla/4.0 (compatible; POPSurf/0.1; Dreamcast)
```

The `Mozilla/4.0` prefix preserves compatibility with servers that send simpler
markup to unknown browsers. Sites can match `POPSurf` when browser-specific
handling is necessary.
