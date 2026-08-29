import java.applet.Applet;
import java.awt.Color;
import java.awt.Event;
import java.awt.Graphics;

/* Java 1.0 event overrides, which is what applets of the period use.
   Click anywhere to drop a marker; the counter proves the handler ran. */
public class Click extends Applet {
    int[] mx = new int[16];
    int[] my = new int[16];
    int n = 0, clicks = 0;
    int lastx = -1, lasty = -1;

    public boolean mouseDown(Event e, int x, int y) {
        clicks++;
        if (n < mx.length) { mx[n] = x; my[n] = y; n++; }
        repaint();
        return true;
    }

    public boolean mouseMove(Event e, int x, int y) {
        lastx = x; lasty = y;
        repaint();
        return true;
    }

    public void paint(Graphics g) {
        g.setColor(new Color(18, 22, 30));
        g.fillRect(0, 0, 300, 200);

        g.setColor(Color.white);
        g.drawString("clicks: " + clicks, 12, 20);

        for (int i = 0; i < n; i++) {
            int v = 255 - i * 12;
            if (v < 60) v = 60;
            g.setColor(new Color(v, 194, 75));
            g.fillOval(mx[i] - 6, my[i] - 6, 13, 13);
            g.setColor(Color.white);
            g.drawOval(mx[i] - 6, my[i] - 6, 12, 12);
        }

        if (lastx >= 0) {
            g.setColor(new Color(90, 170, 210));
            g.drawLine(lastx - 8, lasty, lastx + 8, lasty);
            g.drawLine(lastx, lasty - 8, lastx, lasty + 8);
        }
    }
}
