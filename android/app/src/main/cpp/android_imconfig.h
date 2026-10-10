#pragma once

// Large decoded CADU files can produce more than 65535 chart vertices.
// Use 32-bit ImGui indices so the renderer does not abort on large plots.
#define ImDrawIdx unsigned int
