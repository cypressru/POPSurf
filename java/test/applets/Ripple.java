import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;

/* The period shape exactly: start() spawns a Thread, run() loops forever
   mutating state, calling repaint() and sleeping. If this animates, the
   browser is doing what PlanetWeb did. */
public class Ripple extends Applet implements Runnable {
    Thread t;
    int frame = 0;

    public void start() {
        t = new Thread(this);
        t.start();
    }

    public void run() {
        while (true) {
            frame++;
            repaint();
            try { Thread.sleep(40); } catch (Exception e) { }
        }
    }

    public void paint(Graphics g) {
        g.setColor(Color.black);
        g.fillRect(0, 0, 300, 200);

        double ph = frame * 0.18;

        for (int i = 0; i < 9; i++) {
            double r = 12.0 + i * 11.0 + 8.0 * Math.sin(ph - i * 0.6);
            int rr = (int)r;
            int c = 90 + (int)(90.0 * Math.sin(ph - i * 0.6));
            if (c < 0) c = 0;
            if (c > 255) c = 255;
            g.setColor(new Color(c, 140, 255 - c));
            g.drawOval(150 - rr, 100 - rr / 2, rr * 2, rr);
        }

        int bx = 150 + (int)(100.0 * Math.cos(ph * 0.7));
        g.setColor(Color.orange);
        g.fillOval(bx - 7, 176, 15, 15);
    }
}
