/* MediaTracker, reported rather than drawn: the answers are what matter and a
 * picture cannot show them. Run under a real JDK and under this runtime and
 * the two lines have to read the same.
 *
 * The images are offscreen ones so the class needs no artwork on disc; a
 * tracker treats them the same way it treats a decoded GIF. */
import java.applet.Applet;
import java.awt.Graphics;
import java.awt.Image;
import java.awt.MediaTracker;

public class OffTrack extends Applet {
    public void init() {
        Image a = createImage(16, 16);
        Image b = createImage(16, 16);

        MediaTracker mt = new MediaTracker(this);
        mt.addImage(a, 0);
        mt.addImage(b, 1);

        try {
            mt.waitForID(0);
            mt.waitForAll();
        } catch (InterruptedException e) {
            System.out.println("interrupted");
        }

        System.out.println("checkID " + mt.checkID(0));
        System.out.println("checkAll " + mt.checkAll());
        System.out.println("isErrorAny " + mt.isErrorAny());
        System.out.println("isErrorID " + mt.isErrorID(1));
        System.out.println("statusID " + mt.statusID(0, true));
        System.out.println("statusAll " + mt.statusAll(true));
        System.out.println("COMPLETE " + MediaTracker.COMPLETE);
        System.out.println("LOADING " + MediaTracker.LOADING);
        System.out.println("ABORTED " + MediaTracker.ABORTED);
        System.out.println("ERRORED " + MediaTracker.ERRORED);
    }

    public void paint(Graphics g) { }
}
