// Value-type arrays as NGUI uses them: ref locals into elements, field
// stores through element addresses, a generic growable list of structs
// (BetterList<T>) with ToArray, struct equality operators, Array.Copy.
using System;
using System.Collections.Generic;

struct V2
{
    public float x, y;
    public V2(float x, float y) { this.x = x; this.y = y; }
    public static V2 zero { get { return new V2(0f, 0f); } }
    public static bool operator ==(V2 a, V2 b) { return a.x == b.x && a.y == b.y; }
    public static bool operator !=(V2 a, V2 b) { return !(a == b); }
    public override bool Equals(object o) { return o is V2 && (V2)o == this; }
    public override int GetHashCode() { return x.GetHashCode() ^ y.GetHashCode() << 2; }
    public override string ToString() { return "(" + x + "," + y + ")"; }
}

struct Col { public float r, g, b, a; public Col(float r, float g, float b, float a) { this.r = r; this.g = g; this.b = b; this.a = a; } }

struct RectF
{
    float m_XMin, m_YMin, m_Width, m_Height;
    public RectF(float x, float y, float w, float h) { m_XMin = x; m_YMin = y; m_Width = w; m_Height = h; }
    public float xMin { get { return m_XMin; } set { float xMax = this.xMax; m_XMin = value; m_Width = xMax - m_XMin; } }
    public float xMax { get { return m_Width + m_XMin; } set { m_Width = value - m_XMin; } }
    public float yMin { get { return m_YMin; } set { float yMax = this.yMax; m_YMin = value; m_Height = yMax - m_YMin; } }
    public float yMax { get { return m_Height + m_YMin; } set { m_Height = value - m_YMin; } }
    public override string ToString() { return m_XMin + "," + m_YMin + "," + m_Width + "," + m_Height; }
}

class BetterList<T>
{
    public T[] buffer;
    public int size;
    void AllocateMore()
    {
        T[] array = buffer != null ? new T[Math.Max(buffer.Length << 1, 32)] : new T[32];
        if (buffer != null && size > 0) buffer.CopyTo(array, 0);
        buffer = array;
    }
    public void Add(T item) { if (buffer == null || size == buffer.Length) AllocateMore(); buffer[size++] = item; }
    public T[] ToArray() { T[] r = new T[size]; for (int i = 0; i < size; i++) r[i] = buffer[i]; return r; }
    public T this[int i] { get { return buffer[i]; } set { buffer[i] = value; } }
}

class Sprite { public string name = "x"; public RectF outer = new RectF(0f, 0f, 1f, 1f); public RectF inner = new RectF(0f, 0f, 1f, 1f); public bool rotated; }

static class Program
{
    static RectF ToTex(RectF rect, int width, int height)
    {
        RectF result = rect;
        if ((float)width != 0f && (float)height != 0f)
        {
            result.xMin = rect.xMin / (float)width;
            result.xMax = rect.xMax / (float)width;
            result.yMin = 1f - rect.yMax / (float)height;
            result.yMax = 1f - rect.yMin / (float)height;
        }
        return result;
    }

    static int Main()
    {
        V2[] a = new V2[4];
        V2[] b = new V2[4];
        a[0] = V2.zero;
        a[2] = new V2(1f, -1f);
        a[3] = new V2(1f, -1f);
        a[1].x = 0.25f;
        a[2].x = Math.Max(a[1].x, 1f - 0.125f);
        a[3].y = Math.Min(a[1].y + 0.5f, -1f);
        for (int i = 0; i < 4; i++) b[i] = new V2(i * 0.1f, 1f - i * 0.1f);
        Console.WriteLine(string.Join(" ", Array.ConvertAll(a, v => v.ToString())));

        var verts = new BetterList<V2>();
        var cols = new BetterList<Col>();
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++)
            {
                verts.Add(new V2(a[j + 1].x, b[k].y));
                verts.Add(new V2(b[j].x, a[k + 1].y));
                cols.Add(new Col(0.1f * j, 0.2f, 0.3f, 0.2f));
            }
        V2[] arr = verts.ToArray();
        Col[] carr = cols.ToArray();
        Console.WriteLine(arr.Length + " " + arr[5] + " " + arr[17] + " " + carr[8].r + "," + carr[8].a);
        V2[] copy = new V2[arr.Length];
        Array.Copy(arr, copy, arr.Length);
        Console.WriteLine((copy[7] == arr[7]) + " " + (copy[7] != arr[6]) + " " + Array.IndexOf(arr, arr[4]));

        var sprites = new List<Sprite> { new Sprite { name = "bar", outer = new RectF(2018f, 352f, 8f, 8f), inner = new RectF(2019f, 353f, 6f, 6f) } };
        Sprite s = sprites[0];
        Console.WriteLine(ToTex(s.outer, 2048, 2048) + " | " + ToTex(s.inner, 2048, 2048));
        RectF r = s.outer; r.xMax = 3000f; s.outer = r;
        Console.WriteLine(sprites[0].outer + " " + (s.inner.xMin - s.outer.xMin));

        var dict = new Dictionary<int, V2>();
        dict[65] = new V2(3f, 4f);
        V2 g; Console.WriteLine(dict.TryGetValue(65, out g) + " " + g + " " + dict.ContainsKey(66));
        return 0;
    }
}
