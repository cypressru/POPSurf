/* multianewarray in the shapes javac actually emits.
 *
 * The old handler took exactly two lengths and built int rows regardless of
 * the descriptor, so `new int[n][]` - one length, two dimensions - was refused
 * outright, and `new String[a][b]` came back as arrays of int.
 *
 * Printed rather than drawn so the same run can be diffed against a real JVM.
 */
import java.applet.Applet;
import java.awt.Graphics;

public class MultiArr extends Applet {

    static String check() {
        StringBuffer sb = new StringBuffer();

        /* Two lengths, primitive rows - the case that already worked. */
        int[][] grid = new int[3][4];
        grid[2][3] = 7;
        sb.append("grid=" + grid.length + "x" + grid[0].length
                  + " v=" + grid[2][3] + "\n");

        /* One length of a two-dimensional type. javac emits multianewarray
         * with a count of 1 here, and the rows are left null. */
        int[][] ragged = new int[3][];
        sb.append("ragged=" + ragged.length + " row0="
                  + (ragged[0] == null ? "null" : "set") + "\n");
        ragged[0] = new int[2];
        ragged[0][1] = 5;
        sb.append("filled=" + ragged[0][1] + "\n");

        /* Reference rows. Built as int rows, this stored a pointer into a
         * four-byte element and the read came back as garbage or a crash. */
        String[][] names = new String[2][2];
        names[1][0] = "ok";
        sb.append("names=" + names.length + "x" + names[0].length
                  + " v=" + names[1][0] + "\n");

        /* Three deep, to show the count is not hard-coded anywhere. */
        int[][][] cube = new int[2][3][4];
        cube[1][2][3] = 9;
        sb.append("cube=" + cube.length + "x" + cube[0].length + "x"
                  + cube[0][0].length + " v=" + cube[1][2][3] + "\n");

        /* Rows of a deeper type: two lengths given, third dimension null. */
        int[][][] slab = new int[2][3][];
        sb.append("slab=" + slab.length + "x" + slab[0].length + " row="
                  + (slab[1][2] == null ? "null" : "set") + "\n");

        /* Widths other than int, since the element kind now comes from the
         * descriptor rather than being assumed. */
        double[][] d = new double[2][2];
        d[1][1] = 0.5;
        byte[][] b = new byte[2][2];
        b[0][1] = 3;
        sb.append("d=" + d[1][1] + " b=" + b[0][1] + "\n");

        return sb.toString();
    }

    public static void main(String[] args) {
        System.out.print(check());
    }

    public void init() {
        System.out.print(check());
    }

    public void paint(Graphics g) {
        g.drawString("multiarr", 10, 20);
    }
}
