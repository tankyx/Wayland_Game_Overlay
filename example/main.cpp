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

    std::vector<WaylandOverlay::Shape> shapes;
    { // gradient bar (HP-style), green -> yellow, horizontal
        WaylandOverlay::Shape s;
        s.kind = WaylandOverlay::Shape::Filled;
        s.x0 = 100; s.y0 = 320; s.x1 = 260; s.y1 = 340;
        s.color = 0xFF30FF30; s.color2 = 0xFFFFDD33;
        s.gradient = WaylandOverlay::Shape::Horizontal;
        shapes.push_back(s);
    }
    { // thick line
        WaylandOverlay::Shape s;
        s.kind = WaylandOverlay::Shape::Line;
        s.x0 = 400; s.y0 = 450; s.x1 = 700; s.y1 = 600;
        s.thickness = 3.0f; s.color = 0xFFFF3030;
        shapes.push_back(s);
    }
    { // ellipse with vertical gradient fill
        WaylandOverlay::Shape s;
        s.kind = WaylandOverlay::Shape::Ellipse;
        s.x0 = 500; s.y0 = 200; s.x1 = 60; s.y1 = 40;
        s.color = 0xFF4499FF; s.color2 = 0xFF33DDDD;
        s.gradient = WaylandOverlay::Shape::Vertical;
        shapes.push_back(s);
    }
    { // triangle
        WaylandOverlay::Shape s;
        s.kind = WaylandOverlay::Shape::Triangle;
        s.pts = {800, 300, 900, 300, 850, 380};
        s.color = 0xFFFF44FF;
        shapes.push_back(s);
    }
    { // pentagon
        WaylandOverlay::Shape s;
        s.kind = WaylandOverlay::Shape::Polygon;
        s.pts = {600, 500, 660, 470, 720, 500, 690, 560, 630, 560};
        s.color = 0xFFFF9933;
        shapes.push_back(s);
    }
    overlay.setShapes(shapes);

    overlay.setStats(3.2, 60);
    overlay.setSourceSize(1920, 1080);

    std::this_thread::sleep_for(std::chrono::seconds(3));
    overlay.stop();
    return 0;
}
