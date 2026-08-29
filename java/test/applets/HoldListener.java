import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;

/* An anonymous listener with nothing but the browser holding it, next to an
   animation loop allocating hard enough to force collections.

   `addMouseListener(new MouseAdapter(){...})` leaves no reference anywhere the
   applet can see: the only thing that knows the object exists is the listener
   table on the VM. If that table is not a GC root, the first collection frees
   it and the next click reaches an object that has been handed back to
   malloc - which on a console with no memory protection is not a crash you get
   to debug. So: run for a while, collect several times, then click. */
public class HoldListener extends Applet implements Runnable {
    int marks = 0, frames = 0;
    Thread t;

    public void init() {
        addMouseListener(new MouseAdapter() {
            public void mousePressed(MouseEvent e) {
                marks++;
                System.out.println("still here after " + frames
                                   + " frames: " + e.getX() + "," + e.getY());
                repaint();
            }
        });
    }

    public void start() {
        t = new Thread(this);
        t.start();
    }

    public void run() {
        while (true) {
            /* Garbage, on purpose: a fresh Color per step is what an applet's
               paint loop produces anyway, and it is what makes the collector
               run. */
            for (int i = 0; i < 200; i++) {
                Color junk = new Color(i & 255, 40, 60);
                if (junk.getRed() == 999) System.out.println("never");
            }
            frames++;
            repaint();
            try { Thread.sleep(20); } catch (InterruptedException e) { }
        }
    }

    public void paint(Graphics g) {
        g.setColor(new Color(16, 26, 20));
        g.fillRect(0, 0, 300, 200);
        g.setColor(Color.white);
        g.drawString("frames: " + frames + "  marks: " + marks, 12, 24);
    }
}
