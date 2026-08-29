import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;

/* Allocates hard enough to collect, and builds a string while it draws.

   Nothing about this is an event test. It is here because it is the smallest
   applet that reproduces a use-after-free the collector had: the interpreter
   kept the running frame's operand stack pointer in a register and only wrote
   it back on a call, so a collection triggered by anything in between - a
   `new`, a String constant, a native call - scanned the frame only as far as a
   stale sp and freed whatever had been pushed since. What made it visible was
   `"frames: " + frames` in paint(), which leaves the Graphics on the stack
   while the string chain allocates; the Graphics was collected and the next
   drawing call went through it.

   Sixty-odd collections in a run under this shape. It ran clean before the
   fix too, most of the time, which is exactly why it wants a test rather than
   an eye. Run it under jevents with a sanitiser. */
public class Churn extends Applet implements Runnable {
    int frames = 0;
    Thread t;

    public void start() { t = new Thread(this); t.start(); }

    public void run() {
        while (true) {
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
        g.drawString("frames: " + frames, 12, 24);
    }
}
