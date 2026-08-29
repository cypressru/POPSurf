/* A collection during a native call frees the operand stack's temporaries.
 *
 * Reduced case for an interpreter bug, not a test of anything that works.
 * `"n" + Color.red.getRGB()` puts a StringBuilder on the operand stack and
 * nowhere else, then reads Color.red - which allocates, and can therefore
 * collect. mark_roots scans a frame's operand stack up to fr->sp, and fr->sp
 * is only written when a frame is pushed or suspended, so for the frame that
 * is running it is stale and the StringBuilder is not among the roots. It is
 * freed, and the append that follows writes through the freed pointer.
 *
 * Nothing about this needs a widget; it needs an allocating native call with a
 * live stack temporary above it, which is what string concatenation around any
 * getter is. Run it under a sanitiser - without one it corrupts the heap
 * quietly and may well exit zero.
 */
import java.applet.Applet;
import java.awt.*;

public class GcStack extends Applet {
    public void init() {
        String s = "";

        for(int i = 0; i < 4000; i++)
            s = "n" + Color.red.getRGB();

        System.out.println(s);
    }

    public void paint(Graphics g) {
        g.drawString("done", 10, 10);
    }
}
