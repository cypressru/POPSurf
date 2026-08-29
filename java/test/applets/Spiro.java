import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;

public class Spiro extends Applet {
    public void paint(Graphics g) {
        g.setColor(Color.black);
        g.fillRect(0, 0, 300, 200);

        int cx = 150, cy = 100;
        int[] px = new int[3];
        int[] py = new int[3];

        // Triangle fan: fillPolygon([I[II), double trig, Color constants.
        for (int k = 0; k < 16; k++) {
            double a = k * Math.PI / 8.0;
            px[0] = cx;  py[0] = cy;
            px[1] = cx + (int)(95.0 * Math.cos(a));
            py[1] = cy + (int)(60.0 * Math.sin(a));
            px[2] = cx + (int)(95.0 * Math.cos(a + 0.30));
            py[2] = cy + (int)(60.0 * Math.sin(a + 0.30));
            int v = 40 + k * 9;
            g.setColor(new Color(v, 60 + k * 6, 120));
            g.fillPolygon(px, py, 3);
        }

        // Orbit of dots: sin/cos, integer conversion, per-dot colour.
        for (int i = 0; i < 96; i++) {
            double a = i * Math.PI / 24.0;
            double r = 30.0 + 60.0 * Math.sin(i * Math.PI / 48.0);
            int x = cx + (int)(r * Math.cos(a));
            int y = cy + (int)(r * Math.sin(a) * 0.62);
            int c = 128 + (int)(120.0 * Math.sin(i / 7.0));
            g.setColor(new Color(c, 255 - c, 210));
            g.fillOval(x - 4, y - 4, 9, 9);
        }

        g.setColor(Color.white);
        g.drawString("Math.sin  fillPolygon  Color.white", 10, 190);
        g.setColor(Color.orange);
        g.drawRect(4, 4, 291, 191);
    }
}
