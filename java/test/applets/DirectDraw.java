/* Control for DoubleBuf: the identical picture, drawn straight to the Graphics
 * the browser handed paint() instead of through an offscreen buffer. Any pixel
 * where this disagrees with a real JDK is the rasteriser disagreeing, not the
 * double-buffer path. */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;

public class DirectDraw extends Applet implements Runnable {
    int    frame;
    Thread anim;

    public void start() {
        anim = new Thread(this);
        anim.start();
    }

    public void run() {
        while (true) {
            frame++;
            repaint();
            try { Thread.sleep(50); } catch (InterruptedException e) { return; }
        }
    }

    public void paint(Graphics og) {
        int t = frame;
        int w = 300, h = 200;

        og.setColor(Color.white);
        og.fillRect(0, 0, w, h);

        og.setColor(Color.blue);
        og.fillRect(20 + t * 12, 20, 40, 30);
        og.setColor(Color.black);
        og.drawRect(19 + t * 12, 19, 41, 31);

        og.setColor(Color.red);
        og.drawLine(0, h - 1, w - 1, 0);
        og.drawLine(0, 0, w - 1, h - 1);

        og.setColor(Color.green);
        og.fillRect(10, 150 - t * 6, 280, 8);

        og.setColor(new Color(255, 128, 0));
        og.fillOval(120, 90, 60, 40);
        og.setColor(Color.black);
        og.drawOval(120, 90, 60, 40);
    }
}
