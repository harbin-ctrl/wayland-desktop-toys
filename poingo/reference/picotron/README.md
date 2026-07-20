# Original Picotron Poingo

This directory archives the original public Picotron version of Poingo as a design reference for the modern codebase.

Contents:
- `poingo-4.p64.png`: original Picotron cart image downloaded from Lexaloffle
- `main.lua`: extracted cart source
- `sfx/0.sfx`: extracted sound asset used by the cart

Provenance:
- Source thread/cart: Lexaloffle Picotron BBS, cart `poingo-4` (thread `tid=152033`)
- Cart extraction performed locally with `shrinkotron`

Sound note:
- The original audible "warble" is created primarily by layering two notes at once in `main.lua` on each bounce trigger.
- Wall bounces trigger notes `13` and `11` with duration `16`.
- Floor bounces trigger notes `13` and `11` with duration `32`.
- That small pitch separation creates the beating / warble effect, and the longer floor duration lets it bloom more clearly.

This archive is reference material only. It is not part of the shipped Android/Desktop runtime.
