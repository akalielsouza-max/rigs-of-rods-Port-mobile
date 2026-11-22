package org.rigsofrods.mobile;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

// Copia o conteudo do RoR (recursos + 1 mapa + 1 carro) de dentro do APK para a pasta do app e confere os zips.
public class ContentInstaller {
    public interface Out {
        void line(String s);
    }

    public static File root(Context c) {
        return new File(c.getFilesDir(), "ror");
    }

    public static void install(Context c, Out out) {
        try {
            File dst = root(c);
            String version = "?";
            try {
                PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
                version = pi.versionName;
            } catch (Throwable ignored) {
            }
            File marker = new File(dst, ".versao");
            boolean force = !version.equals(readText(marker));
            long[] stat = new long[2]; // arquivos copiados, bytes copiados
            long t0 = System.currentTimeMillis();
            copyDir(c.getAssets(), "ror", dst, force, stat);
            writeText(marker, version);
            out.line("Copiados " + stat[0] + " arquivos (" + (stat[1] / 1048576) + " MB) em "
                    + (System.currentTimeMillis() - t0) / 1000 + " s. Pasta: " + dst.getPath());
            verify(new File(dst, "resources"), out);
            verify(new File(dst, "content"), out);
        } catch (Throwable t) {
            out.line("ERRO ao instalar o conteudo: " + t);
        }
    }

    private static void copyDir(AssetManager am, String path, File dst, boolean force, long[] stat) throws IOException {
        String[] list = am.list(path);
        if (list != null && list.length > 0) {
            dst.mkdirs();
            for (String name : list) {
                copyDir(am, path + "/" + name, new File(dst, name), force, stat);
            }
            return;
        }
        if (dst.exists() && dst.length() > 0 && !force) {
            return;
        }
        InputStream in = am.open(path);
        OutputStream os = new FileOutputStream(dst);
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) {
            os.write(buf, 0, n);
            stat[1] += n;
        }
        os.close();
        in.close();
        stat[0]++;
    }

    private static void verify(File dir, Out out) {
        File[] files = dir.listFiles();
        if (files == null) {
            out.line("Pasta ausente: " + dir.getPath());
            return;
        }
        java.util.Arrays.sort(files);
        int ok = 0;
        int bad = 0;
        for (File f : files) {
            if (!f.getName().endsWith(".zip")) {
                continue;
            }
            try {
                ZipFile z = new ZipFile(f);
                int n = 0;
                Enumeration<? extends ZipEntry> e = z.entries();
                while (e.hasMoreElements()) {
                    e.nextElement();
                    n++;
                }
                z.close();
                ok++;
                out.line("  OK " + f.getName() + ": " + n + " itens, " + f.length() / 1024 + " KB");
            } catch (Throwable t) {
                bad++;
                out.line("  CORROMPIDO " + f.getName() + ": " + t);
            }
        }
        out.line(dir.getName() + ": " + ok + " zips OK, " + bad + " com problema");
    }

    private static String readText(File f) {
        try {
            if (!f.exists()) {
                return null;
            }
            FileInputStream in = new FileInputStream(f);
            byte[] b = new byte[(int) f.length()];
            int n = in.read(b);
            in.close();
            return new String(b, 0, Math.max(n, 0), "UTF-8");
        } catch (Throwable t) {
            return null;
        }
    }

    private static void writeText(File f, String s) {
        try {
            f.getParentFile().mkdirs();
            FileOutputStream o = new FileOutputStream(f);
            o.write(s.getBytes("UTF-8"));
            o.close();
        } catch (Throwable ignored) {
        }
    }
}
// FIM_DO_ARQUIVO
          
