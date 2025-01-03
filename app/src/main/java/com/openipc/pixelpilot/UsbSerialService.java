package com.openipc.pixelpilot;

import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Handler;
import android.os.IBinder;
import android.util.Log;

import com.hoho.android.usbserial.driver.UsbSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialPort;
import com.hoho.android.usbserial.driver.UsbSerialProber;

import java.io.IOException;
import java.util.List;

public class UsbSerialService extends Service {

    public static native int nativeGetSignalQuality();

    private static final String TAG = "SerialService";
    private static final int INTERVAL_MS = 500;
    private static final int READ_BUFFER_SIZE = 1024; // Buffer for reading
    private UsbSerialPort serialPort;
    private Handler handler = new Handler();
    private UsbManager usbManager;
    private boolean isReading = false;

    private final Runnable sendTask = new Runnable() {
        @Override
        public void run() {
            Log.i(TAG, "USB sending data");
            int quality = nativeGetSignalQuality();
            sendData("/" + quality + "/233/199/\r\n");
            handler.postDelayed(this, INTERVAL_MS);
        }
    };

    private final Runnable readTask = new Runnable() {
        @Override
        public void run() {
            if (serialPort != null) {
                readData();
                handler.postDelayed(this, 100); // Adjust read interval if needed
            }
        }
    };

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if (device == null) return;

            if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
                Log.i(TAG, "USB device attached");
                if (isSerialDevice(device)) {
                    openSerialPort(device);
                }
            } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                Log.i(TAG, "USB device detached");
                if (serialPort != null && serialPort.getDriver().getDevice().equals(device)) {
                    closeSerialPort();
                }
            }
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "USB Service started");
        usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        registerReceiver(usbReceiver, new IntentFilter(UsbManager.ACTION_USB_DEVICE_ATTACHED));
        registerReceiver(usbReceiver, new IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED));
        checkForConnectedDevices();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    private void checkForConnectedDevices() {
        List<UsbSerialDriver> availableDrivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager);
        for (UsbSerialDriver driver : availableDrivers) {
            UsbDevice device = driver.getDevice();
            if (usbManager.hasPermission(device)) {
                Log.i(TAG, "Previously connected device found");
                openSerialPort(device);
                return;
            }
        }
    }

    private boolean isSerialDevice(UsbDevice device) {
        List<UsbSerialDriver> availableDrivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager);
        for (UsbSerialDriver driver : availableDrivers) {
            if (driver.getDevice().equals(device)) {
                return true;
            }
        }
        return false;
    }

    private void openSerialPort(UsbDevice device) {
        List<UsbSerialDriver> availableDrivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager);
        for (UsbSerialDriver driver : availableDrivers) {
            if (driver.getDevice().equals(device)) {
                UsbDeviceConnection connection = usbManager.openDevice(driver.getDevice());
                if (connection == null) {
                    Log.e(TAG, "Failed to open connection");
                    return;
                }

                serialPort = driver.getPorts().get(0);
                try {
                    serialPort.open(connection);
                    serialPort.setParameters(115200, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE);
                    Log.i(TAG, "Serial port opened successfully");
                    handler.post(sendTask);
                    startReading();
                } catch (IOException e) {
                    Log.e(TAG, "Error opening serial port", e);
                }
                return;
            }
        }
        Log.e(TAG, "Attached device is not a serial device");
    }

    private void sendData(String data) {
        if (serialPort == null) {
            Log.e(TAG, "Serial port not opened");
            return;
        }
        try {
            serialPort.write(data.getBytes(), 1000);
            Log.i(TAG, "Sent: " + data);
        } catch (IOException e) {
            Log.e(TAG, "Failed to send data", e);
        }
    }

    private void startReading() {
        if (!isReading) {
            isReading = true;
            handler.post(readTask);
        }
    }

    private void stopReading() {
        isReading = false;
        handler.removeCallbacks(readTask);
    }

    private void readData() {
        if (serialPort == null) {
            Log.e(TAG, "USB Serial port not opened for reading");
            return;
        }
        byte[] buffer = new byte[READ_BUFFER_SIZE];
        try {
            int bytesRead = serialPort.read(buffer, 100);
            if (bytesRead > 0) {
                String receivedData = new String(buffer, 0, bytesRead);
                Log.i(TAG, "USB Received: " + receivedData);
            }
        } catch (IOException e) {
            Log.e(TAG, "USB Failed to read data", e);
        }
    }

    private void closeSerialPort() {
        stopReading();
        handler.removeCallbacks(sendTask);
        if (serialPort != null) {
            try {
                serialPort.close();
                Log.i(TAG, "Serial port closed");
            } catch (IOException e) {
                Log.e(TAG, "Error closing serial port", e);
            }
            serialPort = null;
        }
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        unregisterReceiver(usbReceiver);
        closeSerialPort();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
