/* An applet that asks how big it is from its animation thread.
 *
 * This is the opening move of nearly every animated applet of the period:
 * run() reads size() once to work out where the middle is, then loops.  Six
 * of Sun's own demos do it (TumblingDuke, UnderConstruction, BouncingHeads,
 * Animator, ScrollingImages, NervousText) and none of them does it from
 * paint().
 *
 * It matters because ps_applet.c's try_run() builds its ps_jgfx on its own C
 * stack and hands the address to ps_jvm_init, which parks it in vm->gfx.
 * try_run then returns.  paint() is safe - ps_jvm_paint overwrites vm->gfx
 * with a context that is live for the length of that call - but the animation
 * thread runs from ps_applet_cache_tick, where nothing has refreshed it, so
 * every read of vm->gfx is a read of a dead stack frame.  ps_jgeom's
 * applet_box, setFont, getFont, getFontMetrics and stringWidth all read it.
 *
 * It has to be run through `jrun`, not `japplet`: japplet keeps its ps_jgfx
 * in main(), which outlives everything, so the pointer stays good there and
 * the bug is invisible.  Only ps_applet.c's path has the dangling one.
 *
 *     javac --release 8 -d applets applets/ThreadSize.java
 *     gcc -g -O1 -fsanitize=address -I.. -I../../core -I../../gfx \
 *         -I../../net -I../../vendor jrun.c \
 *         $(ls ../ps_*.c | grep -Ev "ps_(jtest|jdc)\.c") -lm -o jrun
 *     ./jrun applets/ThreadSize.class "" out.ppm 3
 *
 * As committed, jrun never gives an applet's thread a slice at all:
 * ps_applet_cache_tick skips any slot whose box the page has not set, and
 * jrun sets none.  Add
 *
 *     ps_applet_set_box(c, main_url, 0, 200);
 *     ps_applet_set_view(c, 0, 600);
 *
 * before the frame loop and the sanitiser reports stack-use-after-return in
 * applet_box.  Without a sanitiser it reads whatever is on the stack, which
 * is usually plausible and sometimes a segfault - three of Sun's own demos
 * (TumblingDuke, UnderConstruction, BouncingHeads) crash there.
 *
 * That missing box is also why no committed host test has ever exercised the
 * animation thread through the browser's own path, which is why this has sat
 * unseen.
 */
import java.applet.Applet;
import java.awt.Graphics;

public class ThreadSize extends Applet implements Runnable {
    Thread  kicker;
    int     w = -1, h = -1;
    int     frame;

    public void start() {
        kicker = new Thread(this);
        kicker.start();
    }

    public void run() {
        /* The read that matters. In paint() this is answered from a live
         * context; here vm->gfx points into try_run's returned frame. */
        w = size().width;
        h = size().height;
        System.out.println("thread sees " + w + "x" + h);

        while (kicker != null && frame < 8) {
            frame++;
            repaint();
            try { Thread.sleep(40); } catch (InterruptedException e) { }
        }
    }

    /* paint() reads the same two numbers from a context that is alive, so a
     * disagreement between the two lines is the bug made visible without a
     * sanitiser. */
    public void paint(Graphics g) {
        g.drawString("thread " + w + "x" + h, 10, 20);
        g.drawString("paint  " + size().width + "x" + size().height, 10, 40);
    }
}
