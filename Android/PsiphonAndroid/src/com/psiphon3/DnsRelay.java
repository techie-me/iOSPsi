/*
 * Copyright (c) 2012, Psiphon Inc.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

package com.psiphon3;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.Socket;
import java.net.SocketException;
import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

import com.psiphon3.Utils.MyLog;


public class DnsRelay
{
    private final int NUM_THREADS = 10;
    private final int SHUTDOWN_POLL_MILLISECONDS = 100;
    private final int SHUTDOWN_TIMEOUT_MILLISECONDS = 1000;
    private final int MAX_PACKET_SIZE = 1024;

    private final String remoteDnsServerIPAddress;
    private final int remoteDnsPort;
    private final int localDnsPort;
    private boolean stopFlag = false;
    private Thread serverThread = null;
    private DatagramSocket serverSocket = null;

    DnsRelay(
            String remoteDnsServerIPAddress,
            int remoteDnsPort,
            int localDnsPort)
    {
        this.remoteDnsServerIPAddress = remoteDnsServerIPAddress;
        this.remoteDnsPort = remoteDnsPort;
        this.localDnsPort = localDnsPort;
    }
    
    public boolean Start()
    {
        Stop();

        this.stopFlag = false;

        try
        {
            this.serverSocket = new DatagramSocket(this.localDnsPort);
            this.serverSocket.setSoTimeout(SHUTDOWN_POLL_MILLISECONDS);
        }
        catch(SocketException e)
        {
            MyLog.d("Failed to start DNS listener", e);
            return false;
        }
        
        serverThread = new Thread(new Server());
        
        return true;
    }
    
    public void Stop()
    {
        this.stopFlag = true;
        try
        {
            this.serverThread.join();
        }
        catch (InterruptedException e)
        {
        }
        this.serverThread = null;
    }
        
    class Server implements Runnable
    {
        public void run()
        {
            
            byte[] buffer = new byte[MAX_PACKET_SIZE];
            ExecutorService threadPool = Executors.newFixedThreadPool(NUM_THREADS);

            while (!stopFlag)
            {
                try
                {
                    DatagramPacket packet = new DatagramPacket(buffer, buffer.length); 
                    serverSocket.receive(packet);

                    byte[] request = new byte[packet.getLength()];
                    System.arraycopy(buffer, 0, request, 0, packet.getLength());

                    threadPool.submit(new Relayer(
                            serverSocket,
                            packet.getAddress(),
                            packet.getPort(),
                            request));
                }
                catch(InterruptedIOException e)
                {
                    // The SHUTDOWN_POLL_MILLISECONDS timeout, to check the stop flag...
                }
                catch(IOException e)
                {
                    MyLog.d("Failed to receive DNS request", e);
                    // Ignore and resume listening...
                }
            }
            
            threadPool.shutdown();
            try
            {
                threadPool.awaitTermination(SHUTDOWN_TIMEOUT_MILLISECONDS, TimeUnit.MILLISECONDS);
            }
            catch (InterruptedException e)
            {
            }
            threadPool.shutdownNow();
        }        
    }
    
    class Relayer implements Runnable
    {
        private DatagramSocket serverSocket;
        private InetAddress clientAddress;
        private int port;
        private byte[] request;

        Relayer(
            DatagramSocket serverSocket,
            InetAddress clientAddress,
            int port,
            byte[] request)
        {
            this.serverSocket = serverSocket;
            this.clientAddress = clientAddress;
            this.port = port;
            this.request = request;
        }
        
        public void run()
        {
            // NOTE: we don't have a local cache, which we might check
            // here. This is a stateless proxy which allows the client
            // to determine it's own cache policy.
                       
            try
            {
                byte response[] = forwardDnsRequest(request);
                
                this.serverSocket.send(new DatagramPacket(
                        response,
                        response.length,
                        this.clientAddress,
                        this.port));
            }
            catch (IOException e)
            {
                MyLog.d("Failed to send DNS request", e);
                // Ignore and abort response...
            }
        }
        
        private byte[] forwardDnsRequest(byte[] request)
                throws IOException
        {
            Socket socket = null;

            try
            {
                // Note: remote DNS server must support TCP requests. We're making
                // TCP requests as we can't do UDP through the SSH tunnel.
                
                // TEMP: explicit SSH forward or transparent proxy?
                // TEMP: long-term tunnel per worker thread? http://docs.oracle.com/javase/1.5.0/docs/api/java/util/concurrent/ThreadPoolExecutor.html
                socket = new Socket(remoteDnsServerIPAddress, remoteDnsPort);
    
                DataInputStream in = new DataInputStream(socket.getInputStream());
                DataOutputStream out = new DataOutputStream(socket.getOutputStream());
    
                // Need a length prefix for TCP DNS requests
                byte[] prefix = new byte[2];
                prefix[0] = (byte)(request.length & 0xFF);
                prefix[1] = (byte)((request.length >> 8) & 0xFF);
    
                out.write(prefix);
                out.write(request);
                out.flush();
    
                ByteArrayOutputStream responseBuffer = new ByteArrayOutputStream();
    
                int responseByte = -1;
                while ((responseByte = in.read()) != -1)
                {
                    responseBuffer.write(responseByte);
                }
                
                return responseBuffer.toByteArray();
            }
            finally
            {
                if (socket != null)
                {
                    socket.close();
                }
            }
        }
    }
}
