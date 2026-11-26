using System;
using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace LeonidaLauncher;

public static class PpmDecoder
{
    /// <summary>
    /// Reads a P6 binary PPM file and decodes it to a high-performance WPF BitmapSource.
    /// </summary>
    public static BitmapSource? LoadPpm(string filePath, int maxWidth = 0)
    {
        if (!File.Exists(filePath)) return null;

        try
        {
            byte[] data = File.ReadAllBytes(filePath);
            if (data.Length < 10) return null;

            // Magic check
            if (data[0] != (byte)'P' || data[1] != (byte)'6') return null;

            int pos = 2;
            int width = 0, height = 0, maxVal = 0;
            int[] headerVals = new int[3];
            int headerIdx = 0;

            while (headerIdx < 3 && pos < data.Length)
            {
                // Skip whitespace
                while (pos < data.Length && char.IsWhiteSpace((char)data[pos])) pos++;

                // Skip comments
                if (pos < data.Length && data[pos] == (byte)'#')
                {
                    while (pos < data.Length && data[pos] != (byte)'\n' && data[pos] != (byte)'\r') pos++;
                    continue;
                }

                int start = pos;
                while (pos < data.Length && !char.IsWhiteSpace((char)data[pos])) pos++;

                if (start < pos)
                {
                    string numStr = System.Text.Encoding.ASCII.GetString(data, start, pos - start);
                    if (int.TryParse(numStr, out int val))
                    {
                        headerVals[headerIdx++] = val;
                    }
                }
            }

            if (headerIdx < 3) return null;
            width = headerVals[0];
            height = headerVals[1];
            maxVal = headerVals[2];

            // Single whitespace after maxVal
            if (pos < data.Length && char.IsWhiteSpace((char)data[pos])) pos++;

            int expectedBytes = width * height * 3;
            if (data.Length - pos < expectedBytes) return null;

            // Downsample if maxWidth is specified and image exceeds it
            int step = 1;
            if (maxWidth > 0 && width > maxWidth)
            {
                step = 2;
                while (width / step > maxWidth) step++;
            }

            int outWidth = (width + step - 1) / step;
            int outHeight = (height + step - 1) / step;
            int outStride = outWidth * 3;
            byte[] pixelBuffer = new byte[outHeight * outStride];

            for (int y = 0; y < outHeight; y++)
            {
                int srcY = y * step;
                if (srcY >= height) break;
                int srcRowOffset = pos + srcY * width * 3;
                int dstRowOffset = y * outStride;

                if (step == 1)
                {
                    Buffer.BlockCopy(data, srcRowOffset, pixelBuffer, dstRowOffset, outStride);
                }
                else
                {
                    for (int x = 0; x < outWidth; x++)
                    {
                        int srcX = x * step;
                        if (srcX >= width) break;
                        int srcPix = srcRowOffset + srcX * 3;
                        int dstPix = dstRowOffset + x * 3;
                        pixelBuffer[dstPix] = data[srcPix];
                        pixelBuffer[dstPix + 1] = data[srcPix + 1];
                        pixelBuffer[dstPix + 2] = data[srcPix + 2];
                    }
                }
            }

            var bitmap = BitmapSource.Create(
                outWidth,
                outHeight,
                96,
                96,
                PixelFormats.Rgb24,
                null,
                pixelBuffer,
                outStride
            );
            bitmap.Freeze();
            return bitmap;
        }
        catch
        {
            return null;
        }
    }
}
