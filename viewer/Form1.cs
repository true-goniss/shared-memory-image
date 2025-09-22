using System;
using System.Drawing;
using System.Windows.Forms;
using System.Diagnostics;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Threading;


namespace shared_memory_viewer
{
    public partial class Form1 : Form
    {
        [StructLayout(LayoutKind.Sequential)]
        struct MEMORY_BASIC_INFORMATION
        {
            public IntPtr BaseAddress;
            public IntPtr AllocationBase;
            public uint AllocationProtect;
            public UIntPtr RegionSize;
            public uint State;
            public uint Protect;
            public uint Type;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr VirtualQuery(IntPtr lpAddress, out MEMORY_BASIC_INFORMATION lpBuffer, UIntPtr dwLength);

        private long dataCapacity = 0;

        // Windows API
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenFileMapping(uint dwDesiredAccess, bool bInheritHandle, string lpName);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr MapViewOfFile(IntPtr hFileMappingObject, uint dwDesiredAccess, uint dwFileOffsetHigh, uint dwFileOffsetLow, UIntPtr dwNumberOfBytesToMap);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool UnmapViewOfFile(IntPtr lpBaseAddress);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr hObject);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenEvent(uint dwDesiredAccess, bool bInheritHandle, string lpName);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

        // Constants
        const uint FILE_MAP_READ = 0x0004;
        const uint EVENT_MODIFY_STATE = 0x0002;
        const uint SYNCHRONIZE = 0x00100000;
        const uint INFINITE = 0xFFFFFFFF;
        const uint WAIT_OBJECT_0 = 0;
        const uint WAIT_TIMEOUT = 0x102;

        private string mapName = "MySharedMemory";
        private string eventName = "Global\\SHM_EV_MySharedMemory";

        const uint MAGIC = 0x5348444D; // 'SHDM'
        const int HEADER_SIZE = 128;

        // Header structure (same as node module)
        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        struct SharedHeader
        {
            public uint magic;
            public uint version;
            public int seq;
            public uint width;
            public uint height;
            public uint channels;
            public uint frame_size;
            public ulong frame_index;
            public ulong mapping_size;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 48)]
            public byte[] reserved;
        }

        private IntPtr hMap = IntPtr.Zero;
        private IntPtr baseAddress = IntPtr.Zero;
        private IntPtr hEvent = IntPtr.Zero;
        private PictureBox pictureBox;
        private Thread renderThread;
        private bool isRunning = false;

        // metadata
        private int width = 800;
        private int height = 600;
        private int channels = 3;
        private int stride;

        public Form1()
        {
            InitializeComponent();
            this.FormClosing += Form1_FormClosing;

            this.Text = "Shared Memory Viewer";
            this.Size = new Size(800, 600);

            pictureBox = new PictureBox
            {
                Dock = DockStyle.Fill,
                SizeMode = PictureBoxSizeMode.Zoom,
                BackColor = Color.Black
            };
            this.Controls.Add(pictureBox);
        }

        protected override void OnLoad(EventArgs e)
        {
            base.OnLoad(e);

            if (InitializeSharedMemory())
            {
                StartRendering();
            }
            else
            {
                MessageBox.Show(
                    "Failed to initialize shared memory. Make sure the writer is running.", 
                    "Error", 
                    MessageBoxButtons.OK, 
                    MessageBoxIcon.Error
                );

                this.Close();
            }
        }

        private void Form1_FormClosing(object sender, FormClosingEventArgs e)
        {
            StopRendering();
            Cleanup();
        }

        private bool InitializeSharedMemory()
        {
            // Open shared memory
            hMap = OpenFileMapping(FILE_MAP_READ, false, mapName);
            if (hMap == IntPtr.Zero)
            {
                MessageBox.Show($"Failed to open file mapping: {Marshal.GetLastWin32Error()}", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }

            // Map view of file
            baseAddress = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, UIntPtr.Zero);
            if (baseAddress == IntPtr.Zero)
            {
                MessageBox.Show($"Failed to map view of file: {Marshal.GetLastWin32Error()}", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                CloseHandle(hMap);
                hMap = IntPtr.Zero;
                return false;
            }

            // Open notify event
            hEvent = OpenEvent(SYNCHRONIZE | EVENT_MODIFY_STATE, false, eventName);
            if (hEvent == IntPtr.Zero)
            {
                // Ain't fatal
                Debug.WriteLine("Could not open event, will use polling");
            }

            // cache the header
            SharedHeader header = ReadHeader();
            if (header.magic != MAGIC)
            {
                MessageBox.Show("Invalid magic in shared memory header", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }

            width = (int)header.width;
            height = (int)header.height;
            channels = (int)header.channels;
            stride = width * channels;

            return true;
        }

        private bool UpdateCapacityFromView()
        {
            try
            {
                if (baseAddress == IntPtr.Zero) { dataCapacity = 0; return false; }
                MEMORY_BASIC_INFORMATION mbi;
                UIntPtr size = (UIntPtr)Marshal.SizeOf(typeof(MEMORY_BASIC_INFORMATION));
                IntPtr res = VirtualQuery(baseAddress, out mbi, size);
                if (res == IntPtr.Zero) { dataCapacity = 0; return false; }
                ulong regionSize = mbi.RegionSize.ToUInt64();
                if (regionSize <= (ulong)HEADER_SIZE) { dataCapacity = 0; return false; }
                dataCapacity = (long)regionSize - HEADER_SIZE;
                return true;
            }
            catch
            {
                dataCapacity = 0;
                return false;
            }
        }

        // helper: trying to reopen mapping (on errors)
        private bool ReopenMapping()
        {
            try
            {
                // cleanup current mapping/view
                if (baseAddress != IntPtr.Zero) { UnmapViewOfFile(baseAddress); baseAddress = IntPtr.Zero; }
                if (hMap != IntPtr.Zero) { CloseHandle(hMap); hMap = IntPtr.Zero; }
                
                // open again
                hMap = OpenFileMapping(FILE_MAP_READ, false, mapName);
                if (hMap == IntPtr.Zero) return false;
                baseAddress = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, UIntPtr.Zero);
                if (baseAddress == IntPtr.Zero) { CloseHandle(hMap); hMap = IntPtr.Zero; return false; }
                UpdateCapacityFromView();
                return true;
            }
            catch
            {
                return false;
            }
        }

        private SharedHeader ReadHeader()
        {
            return Marshal.PtrToStructure<SharedHeader>(baseAddress);
        }

        private void StartRendering()
        {
            isRunning = true;
            renderThread = new Thread(RenderLoop);
            renderThread.IsBackground = true;
            renderThread.Start();
        }

        private void StopRendering()
        {
            isRunning = false;
            if (renderThread != null && renderThread.IsAlive)
            {
                renderThread.Join(1000);
            }
        }

        private void RenderLoop()
        {
            ulong lastFrameIndex = 0;
            byte[] imageData = null;

            UpdateCapacityFromView();

            while (isRunning)
            {
                // Wait / polling
                if (hEvent != IntPtr.Zero)
                {
                    uint result = WaitForSingleObject(hEvent, 100);
                    if (result == WAIT_TIMEOUT)
                    {
                        Thread.Sleep(1);
                        continue;
                    }
                }
                else
                {
                    Thread.Sleep(16);
                }

                // seqlock read header
                SharedHeader header = default;
                int start, end = 0;
                uint frameBytes = 0;
                ulong frameIndex = 0;
                int attempts = 0;
                const int maxAttempts = 10;

                do
                {
                    if (++attempts > maxAttempts) { Debug.WriteLine("Failed to read stable frame"); break; }
                    header = ReadHeader();
                    start = header.seq;
                    if ((start & 1) != 0) { Thread.Sleep(0); continue; }
                    frameBytes = header.frame_size;
                    frameIndex = header.frame_index;
                    Thread.MemoryBarrier();
                    header = ReadHeader();
                    end = header.seq;
                } while (start != end);

                // update cached metadata from header!
                int newWidth = (int)header.width;
                int newHeight = (int)header.height;
                int newChannels = (int)header.channels;

                // check header format
                if (newWidth <= 0 || newHeight <= 0 || (newChannels != 3 && newChannels != 4))
                {
                    Debug.WriteLine($"Invalid header format: w={newWidth}, h={newHeight}, ch={newChannels} — skipping frame");
                    // trying to reopen on invalid format
                    ReopenMapping();
                    continue;
                }

                bool fmtChanged = (width != newWidth) || (height != newHeight) || (channels != newChannels);
                width = newWidth;
                height = newHeight;
                channels = newChannels;
                stride = width * channels;

                // recompute expected bytes / check frameBytes
                long expected = (long)width * (long)height * (long)channels;
                Debug.WriteLine($"HDR: w={width} h={height} ch={channels} frame_size={frameBytes} capacity={dataCapacity} lastIdx={header.frame_index}");
                Debug.WriteLine($"Expected bytes = {expected}");
                if (frameBytes != expected)
                {
                    Debug.WriteLine("WARNING: frame_size != expected -- skipping frame (possible race or writer bug)");
                    ReopenMapping();
                    continue;
                }

                // check magic
                if (header.magic != MAGIC)
                {
                    Debug.WriteLine("Invalid magic; attempting reopen...");
                    ReopenMapping();
                    Thread.Sleep(5);
                    continue;
                }

                // check that frameBytes fits into capacity
                if (dataCapacity == 0) UpdateCapacityFromView();

                if (frameBytes == 0)
                {
                    // nothing to do
                    continue;
                }

                if (frameBytes > (uint)dataCapacity)
                {
                    Debug.WriteLine($"Frame size {frameBytes} > capacity {dataCapacity}; attempting reopen");
                    bool ok = ReopenMapping();
                    if (!ok)
                    {
                        Thread.Sleep(5);
                    }
                    continue;
                }

                // If we've already seen this frame index, skip
                if (frameIndex == lastFrameIndex) continue;
                lastFrameIndex = frameIndex;

                // prepare buffer
                try
                {
                    if (imageData == null || imageData.Length != (int)frameBytes)
                        imageData = new byte[frameBytes];

                    IntPtr dataPtr = IntPtr.Add(baseAddress, HEADER_SIZE);

                    Debug.WriteLine($"HDR: w={header.width} h={header.height} ch={header.channels} frame_size={frameBytes} capacity={dataCapacity} lastIdx={header.frame_index}");


                    Debug.WriteLine($"Expected bytes = {expected}");
                    if (frameBytes != expected)
                    {
                        Debug.WriteLine("WARNING: frame_size != expected -- skipping frame");
                        
                        // recreate suspected -> reconnect
                        ReopenMapping();
                        continue;
                    }
                    // Copy guarded by capacity check above
                    Marshal.Copy(dataPtr, imageData, 0, (int)frameBytes);

                    // swap R <-> B if needed
                    if (channels >= 3)
                    {
                        for (int i = 0; i + 2 < frameBytes; i += channels)
                        {
                            byte tmp = imageData[i];
                            imageData[i] = imageData[i + 2];
                            imageData[i + 2] = tmp;
                        }
                    }
                }
                catch (AccessViolationException ave)
                {
                    Debug.WriteLine("AccessViolation during Marshal.Copy: " + ave.Message);
                    
                    // Reopen, skip frame
                    ReopenMapping();
                    continue;
                }
                catch (Exception ex)
                {
                    Debug.WriteLine("Error copying frame: " + ex.Message);
                    continue;
                }

                // Build bitmap copy via LockBits
                this.Invoke(new Action(() =>
                {
                    try
                    {
                        int w = width;
                        int h = height;
                        int bytesPerPixel = (channels == 3) ? 3 : 4;
                        long expectedBytes = (long)w * h * bytesPerPixel;
                        if (imageData == null || imageData.Length < expectedBytes)
                        {
                            Debug.WriteLine("Image buffer too small for expected size; skipping frame");
                            return;
                        }

                        PixelFormat pf = (channels == 3) ? PixelFormat.Format24bppRgb : PixelFormat.Format32bppArgb;
                        Bitmap clone = new Bitmap(w, h, pf);
                        var rect = new Rectangle(0, 0, w, h);
                        var bmpData = clone.LockBits(rect, ImageLockMode.WriteOnly, pf);
                        try
                        {
                            int srcStride = w * bytesPerPixel;
                            int dstStride = Math.Abs(bmpData.Stride);
                            IntPtr dstScan0 = bmpData.Scan0;

                            // if stride matches, then the easy way
                            if (dstStride == srcStride)
                            {
                                Marshal.Copy(imageData, 0, dstScan0, (int)expectedBytes);
                            }
                            else
                            {
                                // keep the negative dst.Stride (top-down)
                                bool dstTopDown = bmpData.Stride < 0;
                                IntPtr dstRowPtr = dstTopDown
                                    ? IntPtr.Add(dstScan0, (h - 1) * dstStride) // start from the last line
                                    : dstScan0;

                                for (int y = 0; y < h; y++)
                                {
                                    int srcOffset = y * srcStride;
                                    Marshal.Copy(imageData, srcOffset, dstRowPtr, srcStride);

                                    // move the pointer to the next line up or down
                                    dstRowPtr = dstTopDown ? IntPtr.Subtract(dstRowPtr, dstStride) : IntPtr.Add(dstRowPtr, dstStride);
                                }
                            }
                        }
                        finally
                        {
                            clone.UnlockBits(bmpData);
                        }

                        // swap-picture atomically
                        var old = pictureBox.Image;
                        pictureBox.Image = clone;
                        old?.Dispose();
                    }
                    catch (Exception ex)
                    {
                        Debug.WriteLine("Error creating bitmap in UI thread: " + ex.ToString());
                    }
                }));
            }
        }

        private void Cleanup()
        {
            if (baseAddress != IntPtr.Zero)
            {
                UnmapViewOfFile(baseAddress);
                baseAddress = IntPtr.Zero;
            }

            if (hMap != IntPtr.Zero)
            {
                CloseHandle(hMap);
                hMap = IntPtr.Zero;
            }

            if (hEvent != IntPtr.Zero)
            {
                CloseHandle(hEvent);
                hEvent = IntPtr.Zero;
            }
        }
    }
}