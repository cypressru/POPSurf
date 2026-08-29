/* Reduced from a 1999 applet whose classes inherit from one another.
 *
 * super(...) where the subclass constructor has the same descriptor as the
 * superclass one. invokespecial must resolve statically; dispatching it on
 * the receiver's class finds SDerived.<init>(I) again.
 *
 * Correct output is "tag=8". The runtime prints "tag=0" and exits green,
 * which is the reason this file exists: the failure is silent.
 */
import java.applet.Applet;
import java.awt.Graphics;

class SBase {
    int tag;

    SBase(int n) { tag = n; }
}

class SDerived extends SBase {
    SDerived(int n) { super(n + 1); }
}

public class SuperCtor extends Applet {
    public void paint(Graphics g) {
        SDerived d = new SDerived(7);

        g.drawString("tag " + d.tag, 10, 20);
        System.out.println("tag=" + d.tag);
    }
}
