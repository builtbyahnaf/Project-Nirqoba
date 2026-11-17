public class Main {
    static class TreeNode {
        int val;
        TreeNode left;
        TreeNode right;
        TreeNode(int v, TreeNode l, TreeNode r) {
            this.val = v;
            this.left = l;
            this.right = r;
        }
    }

    static TreeNode buildTree(int depth, int idx) {
        if (depth == 0) return null;
        return new TreeNode(idx, buildTree(depth - 1, idx * 2), buildTree(depth - 1, idx * 2 + 1));
    }

    static long preorder(TreeNode node) {
        if (node == null) return 0L;
        return node.val + preorder(node.left) + preorder(node.right);
    }

    static long inorder(TreeNode node) {
        if (node == null) return 0L;
        return inorder(node.left) + node.val + inorder(node.right);
    }

    static long postorder(TreeNode node) {
        if (node == null) return 0L;
        return postorder(node.left) + postorder(node.right) + node.val;
    }

    public static void main(String[] args) {
        int depth = 20; // 1,048,575 nodes
        if (args.length > 0) {
            depth = Integer.parseInt(args[0]);
        }

        long t0 = System.nanoTime();
        TreeNode root = buildTree(depth, 1);
        long t1 = System.nanoTime();
        double buildMs = (t1 - t0) / 1_000_000.0;

        long t2 = System.nanoTime();
        long sumPre = preorder(root);
        long t3 = System.nanoTime();
        double preMs = (t3 - t2) / 1_000_000.0;

        long t4 = System.nanoTime();
        long sumIn = inorder(root);
        long t5 = System.nanoTime();
        double inMs = (t5 - t4) / 1_000_000.0;

        long t6 = System.nanoTime();
        long sumPost = postorder(root);
        long t7 = System.nanoTime();
        double postMs = (t7 - t6) / 1_000_000.0;

        double totalMs = buildMs + preMs + inMs + postMs;
        System.out.println("WORKLOAD: ST4-12");
        System.out.println("LANGUAGE: Java");
        System.out.println("NODES: " + ((1 << depth) - 1));
        System.out.printf("BUILD_MS: %.3f\n", buildMs);
        System.out.printf("PREORDER_MS: %.3f\n", preMs);
        System.out.printf("INORDER_MS: %.3f\n", inMs);
        System.out.printf("POSTORDER_MS: %.3f\n", postMs);
        System.out.printf("TIME_MS: %.3f\n", totalMs);
        System.out.println("CHECKSUM: " + sumPre);
        System.out.println("STATUS: " + (sumPre == sumIn && sumIn == sumPost ? "PASS" : "FAIL"));
    }
}
