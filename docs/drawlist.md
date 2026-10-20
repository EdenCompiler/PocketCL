# Drawlist format

**Status: reserved.** The roadmap calls for Lisp to emit a flat byte-vector drawlist and for C to consume it. M7 defines the operation set and byte encoding alongside the headless renderer. Those details are not implemented or specified here yet.

When the format is established, document operation layouts, integer units and bounds, text and clipping behavior, validation, and readable golden dumps. Keep drawing decisions in Lisp; the shared C rasterizer should execute the defined pixel operations without introducing UI policy.
