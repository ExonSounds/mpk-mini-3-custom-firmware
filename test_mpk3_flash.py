import os
import unittest

from mpk3_flash import STOCK, check_image, cmd, data_packet, flash

STOCK_IMG = open(STOCK, "rb").read()


class FakeDevice:
    """Records packets and answers like the bootloader: UI F<n> (ok), or E<n> (fail) for `fail_on`."""

    def __init__(self, fail_on=None):
        self.sent, self.fail_on = [], fail_on
        self.r, self.w = os.pipe()

    def write(self, data):
        assert data[0] == 0 and len(data) == 33
        pkt = data[1:]
        self.sent.append(pkt)
        status = 0xE0 if pkt[2] == self.fail_on else 0xF0
        os.write(self.w, b"UI" + bytes([status | pkt[2] & 0x0F, 0x01, 0x26]) + bytes(27))
        return len(data)


class FlashTest(unittest.TestCase):
    def test_stock_image_passes_checks(self):
        self.assertEqual(check_image(STOCK_IMG, STOCK_IMG), [])

    def test_rejects_bad_checksum_boot_change_and_size(self):
        app = bytearray(STOCK_IMG)
        app[0xB000] ^= 1
        self.assertIn("checksum", check_image(bytes(app), STOCK_IMG)[0])
        boot = bytearray(STOCK_IMG)
        boot[0], boot[1] = boot[1], boot[0]  # swap keeps the checksum, so only the boot check fires
        errs = check_image(bytes(boot), STOCK_IMG)
        self.assertEqual(len(errs), 1, errs)
        self.assertIn("boot region", errs[0])
        self.assertIn("bytes", check_image(STOCK_IMG[:-1], STOCK_IMG)[0])

    def test_data_packet_layout(self):
        chunk = bytes(range(0xF0, 0x100))
        p = data_packet(0x1234, 0x0102, chunk)
        self.assertEqual(len(p), 32)
        self.assertEqual(p[:9].hex(), "554fc2000012340102")
        self.assertEqual(p[9:25], chunk)
        self.assertEqual(p[25:30], bytes(5))
        self.assertEqual(int.from_bytes(p[30:], "big"), sum(chunk))
        self.assertEqual(cmd(0xC1), b"\x55\x4f\xc1" + bytes(29))

    def _run(self, dev):
        real_write = os.write
        os.write = lambda fd, data: dev.write(data) if fd == dev.r else real_write(fd, data)
        try:
            flash(dev.r, STOCK_IMG, progress=lambda *_: None)
        finally:
            os.write = real_write

    def test_full_sequence(self):
        dev = FakeDevice()
        self._run(dev)
        codes = [p[2] for p in dev.sent]
        self.assertEqual(codes[:3], [0xC4, 0xC1, 0xC4])  # connect, erase, version
        self.assertEqual(codes[-1], 0xC3)
        data = dev.sent[3:-1]
        self.assertEqual(len(data), 0x20000 // 16)
        self.assertEqual(b"".join(p[9:25] for p in data), STOCK_IMG)
        self.assertEqual(data[-1][3:9].hex(), "0001fff01fff")

    def test_connect_refused_never_erases(self):
        dev = FakeDevice(fail_on=0xC4)  # bootloader answers UI E4
        with self.assertRaisesRegex(RuntimeError, "did not accept connect"):
            self._run(dev)
        self.assertEqual([p[2] for p in dev.sent], [0xC4])

    def test_erase_error_aborts_before_data(self):
        dev = FakeDevice(fail_on=0xC1)
        with self.assertRaisesRegex(RuntimeError, "rejected command 0xc1"):
            self._run(dev)
        self.assertEqual([p[2] for p in dev.sent], [0xC4, 0xC1])


if __name__ == "__main__":
    unittest.main()
