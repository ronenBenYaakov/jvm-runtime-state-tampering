@echo off
setlocal
title TCP Workload Generator

:: Number of requests to send (change as needed)
set COUNT=10
:: Delay between requests in milliseconds
set DELAY_MS=500

echo ======================================================
echo Sending %COUNT% WORK requests to TCP Server (127.0.0.1:9000)...
echo ======================================================
echo.

powershell -NoProfile -Command ^
    "$port = 9000;" ^
    "$hostName = '127.0.0.1';" ^
    "for ($i = 1; $i -le %COUNT%; $i++) {" ^
    "    try {" ^
    "        $client = New-Object System.Net.Sockets.TcpClient($hostName, $port);" ^
    "        $stream = $client.GetStream();" ^
    "        $writer = New-Object System.IO.StreamWriter($stream);" ^
    "        $reader = New-Object System.IO.StreamReader($stream);" ^
    "        $writer.WriteLine('WORK');" ^
    "        $writer.Flush();" ^
    "        $response = $reader.ReadLine();" ^
    "        Write-Host \"[Request $i/%COUNT%] Server Response: $response\" -ForegroundColor Green;" ^
    "        $client.Close();" ^
    "        Start-Sleep -Milliseconds %DELAY_MS%;" ^
    "    } catch {" ^
    "        Write-Host \"[!] Failed to connect to server on port $port\" -ForegroundColor Red;" ^
    "        break;" ^
    "    }" ^
    "}"

echo.
echo ======================================================
echo Done. Press any key to exit.
echo ======================================================
pause >nul