/* The offscreen double-buffer, in the shape every animated applet of the
 * period writes it: allocate the back buffer once in init(), keep the Graphics
 * that draws into it, blit the buffer in paint().
 *
 * Written so the same class runs under a real JDK as well as under this
 * runtime, which is how the output is checked. paint() is a pure function of
 * the frame counter and the thread is the only thing that advances it: under a
 * real AWT the event queue calls paint() whenever it likes, and a counter
 * incremented by paint() would run ahead of ours by however many of those
 * happened to land.
 *
 * Shapes only, deliberately. Text would compare our glyph cache against the
 * host JDK's font, which says nothing about double buffering.
 */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;
import java.awt.Image;
import java.awt.MediaTracker;

public class DoubleBuf extends Applet implements Runnable {
    Image    off;
    Graphics og;
    int      frame;
    Thread   anim;

    public void init() {
        off = createImage(300, 200);
        og  = off.getGraphics();

        /* The tracker has nothing to wait for here, which is the point: it has
         * to be constructible and its waits have to come back. */
        MediaTracker mt = new MediaTracker(this);
        mt.addImage(off, 0);
        try { mt.waitForID(0); } catch (InterruptedException e) { }
    }

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

    public void paint(Graphics g) {
        int t = frame;
        int w, h;

        /* A real AWT can call paint() before init() has run. */
        if (og == null)
            return;

        w = off.getWidth(this);
        h = off.getHeight(this);

        og.setColor(Color.white);
        og.fillRect(0, 0, w, h);

        /* Sliding block with an outline, which is where drawRect's inclusive
         * span shows up if it is wrong. */
        og.setColor(Color.blue);
        og.fillRect(20 + t * 12, 20, 40, 30);
        og.setColor(Color.black);
        og.drawRect(19 + t * 12, 19, 41, 31);

        /* Both diagonals, endpoints included. */
        og.setColor(Color.red);
        og.drawLine(0, h - 1, w - 1, 0);
        og.drawLine(0, 0, w - 1, h - 1);

        og.setColor(Color.green);
        og.fillRect(10, 150 - t * 6, 280, 8);

        og.setColor(new Color(255, 128, 0));
        og.fillOval(120, 90, 60, 40);
        og.setColor(Color.black);
        og.drawOval(120, 90, 60, 40);

        g.drawImage(off, 0, 0, this);
    }
}
