// Standalone demo: open the overlay on the largest output, paint a few boxes,
// a line, a filled rect, and a stats line for ~3 s, then shut down.
#include <WaylandOverlay.h>

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    WaylandOverlay overlay; // 0x0 -> falls back to the largest output
    if (!overlay.start()) {
        std::fprintf(stderr, "overlay failed to start (no Wayland session?)\n");
        return 1;
    }

    overlay.setLabelNames({"hostile", "friendly", "neutral"});

    std::vector<WaylandOverlay::DetectionBox> boxes = {
        {100, 100, 120, 200, 0, 0.91f},
        {400, 250, 80, 150, 1, 0.87f},
        {700, 500, 60, 90, 2, 0.73f},
    };
    overlay.setDetections(boxes);

    std::vector<WaylandOverlay::Shape> shapes = {
        {WaylandOverlay::Shape::Filled, 100, 320, 260, 340, 0, 0xFF30FF30}, // bar
        {WaylandOverlay::Shape::Line, 400, 450, 700, 600, 3.0f, 0xFFFF3030}, // line
    };
    overlay.setShapes(shapes);

    overlay.setStats(3.2, 60);
    overlay.setSourceSize(1920, 1080);

    std::this_thread::sleep_for(std::chrono::seconds(3));
    overlay.stop();
    return 0;
}
