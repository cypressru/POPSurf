/* A helper class, in its own file. The applet references it, so the browser
   has to notice and fetch it before anything can run. */
import java.awt.Color;
import java.awt.Graphics;

public class Sprite {
    int x, y, dx, dy, size;
    Color c;

    Sprite(int x, int y, int dx, int dy, int size, Color c) {
        this.x = x; this.y = y; this.dx = dx; this.dy = dy;
        this.size = size; this.c = c;
    }

    void step(int w, int h) {
        x += dx; y += dy;
        if (x < 0 || x > w - size) dx = -dx;
        if (y < 0 || y > h - size) dy = -dy;
    }

    void draw(Graphics g) {
        g.setColor(c);
        g.fillOval(x, y, size, size);
        g.setColor(Color.white);
        g.drawOval(x, y, size - 1, size - 1);
    }
}
