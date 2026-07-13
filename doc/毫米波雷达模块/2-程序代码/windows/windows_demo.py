import serial
import binascii


def send_hex_string(serial_port, hex_string):
    hex_bytes = binascii.unhexlify(hex_string)
    serial_port.write(hex_bytes)


def read_serial_data(serial_port):
    while True:
        data = serial_port.readline().decode('utf-8', errors='ignore').strip()
        print(data)


if __name__ == "__main__":
    ser = serial.Serial('COM6', 115200, timeout=1)

    hex_to_send = "FDFCFBFA0800120000006400000004030201"
    send_hex_string(ser, hex_to_send)

    read_serial_data(ser)

    ser.close()
