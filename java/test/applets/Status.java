/* showStatus, and the two info methods a browser may ask an applet for.
 *
 * None of these draws anything and none of them can be seen on a Dreamcast:
 * there is no status line. They matter because an applet narrating itself
 * does it from init(), so a missing showStatus killed the applet on its
 * first line - five of them, across three authors.
 */
import java.applet.Applet;
import java.awt.Graphics;

public class Status extends Applet {

    public void init() {
        showStatus("loading");
        showStatus("");
        showStatus(null);
        System.out.println("info=" + getAppletInfo());
        System.out.println("params=" + (getParameterInfo() == null
                                        ? "null" : "some"));
        showStatus("ready");
        System.out.println("survived");
    }

    public void paint(Graphics g) {
        g.drawString("status", 10, 20);
    }
}
