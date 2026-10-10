package io.github.ggak.decoder;

import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.database.Cursor;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public final class MainActivity extends NativeActivity {
    private static final int OPEN_CADU = 1001;

    static { System.loadLibrary("ggak_android"); }
    private static native void nativeSetFile(String path, String displayName);

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
    }

    public void openCaduPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/octet-stream");
        startActivityForResult(intent, OPEN_CADU);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != OPEN_CADU || result != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        String name = queryName(uri);
        File cached = new File(getCacheDir(), "selected.cadu");
        try (InputStream in = getContentResolver().openInputStream(uri);
             FileOutputStream out = new FileOutputStream(cached, false)) {
            if (in == null) return;
            byte[] buffer = new byte[65536];
            int count;
            while ((count = in.read(buffer)) >= 0) out.write(buffer, 0, count);
            nativeSetFile(cached.getAbsolutePath(), name);
        } catch (Exception error) {
            nativeSetFile("", "读取文件失败: " + error.getMessage());
        }
    }

    private String queryName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) return cursor.getString(column);
            }
        }
        return "selected.cadu";
    }
}
