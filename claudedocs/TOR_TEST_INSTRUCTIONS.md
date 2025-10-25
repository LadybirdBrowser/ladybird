# Testing Tor Integration - Quick Start Guide

## Status Check

✅ Tor service installed and running (listening on 127.0.0.1:9050)

## Option 1: Quick Test with Temporary Auto-Enable (Recommended)

This will automatically enable Tor for ALL network requests, making it easy to test.

### Step 1: Add Temporary Auto-Enable Code

Add this line to `Services/RequestServer/ConnectionFromClient.cpp` in the constructor:

**Location**: Line 394 (right after `s_connections.set(client_id(), *this);`)

```cpp
ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<RequestClientEndpoint, RequestServerEndpoint>(*this, move(transport), s_client_ids.allocate())
    , m_resolver(default_resolver())
{
    s_connections.set(client_id(), *this);

    // TEMPORARY TEST: Auto-enable Tor for all connections
    enable_tor();  // ADD THIS LINE

    m_alt_svc_cache_path = ByteString::formatted("{}/Ladybird/alt-svc-cache.txt", Core::StandardPaths::user_data_directory());
    // ... rest of constructor
}
```

### Step 2: Build Ladybird

```bash
cd /mnt/c/Development/Projects/ladybird/ladybird
./Meta/ladybird.py build
```

### Step 3: Run Ladybird

```bash
./Meta/ladybird.py run
```

### Step 4: Test Tor Integration

In the browser, navigate to:
- https://check.torproject.org

**Expected Result**: Should see "Congratulations. This browser is configured to use Tor."

**IP Check**: The displayed IP should be a Tor exit node IP, not your real IP.

### Step 5: Verify Stream Isolation (Advanced)

Open multiple tabs and navigate each to https://check.torproject.org

**Expected**: Different tabs show different Tor exit IPs (proving stream isolation works)

### Step 6: Remove Test Code

After testing, remove the `enable_tor();` line you added in Step 1.

---

## Option 2: Test via Command Line curl (Verify Tor is Working)

Before testing Ladybird, verify Tor itself works:

```bash
# Test direct connection (shows your real IP)
curl https://check.torproject.org/api/ip

# Test through Tor (shows Tor exit IP)
curl --socks5-hostname localhost:9050 https://check.torproject.org/api/ip
```

If the second command shows a different IP, Tor is working correctly.

---

## Option 3: Test via gdb (For Debugging)

```bash
cd /mnt/c/Development/Projects/ladybird/ladybird
./Meta/ladybird.py gdb ladybird

# In gdb:
(gdb) break ConnectionFromClient::issue_network_request
(gdb) run

# When breakpoint hits:
(gdb) print this->m_network_identity
(gdb) print this->m_network_identity->has_proxy()
(gdb) continue
```

---

## What to Look For

### Success Indicators:

1. **Tor Check Page**: "Congratulations. This browser is configured to use Tor."
2. **Different IP**: IP shown is NOT your real IP
3. **Debug Output**: If REQUESTSERVER_DEBUG is enabled, you'll see:
   ```
   RequestServer: Tor enabled for client X with circuit Y
   RequestServer: Using proxy socks5h://localhost:9050 for request to ...
   ```

### Failure Indicators:

1. **Tor Check Page**: "Sorry. You are not using Tor."
2. **Same IP**: IP shown is your real IP
3. **Connection Errors**: Can't connect to sites

### Troubleshooting:

**Problem**: "Unable to connect"
- **Solution**: Verify Tor is running: `sudo systemctl status tor`

**Problem**: "You are not using Tor"
- **Solution**: Check if proxy code is being executed (add debug prints)
- Verify `enable_tor()` was called

**Problem**: Build errors
- **Solution**: Check for typos in added code
- Verify all files are saved

---

## Next Steps After Successful Test

Once you confirm Tor integration works:

1. Remove temporary auto-enable code
2. Add UI controls (Tor toggle button per tab)
3. Add IPC messages for WebContent communication
4. Implement "New Identity" button (circuit rotation)

---

## Expected Debug Output

With `REQUESTSERVER_DEBUG` enabled, you should see:

```
RequestServer: Tor enabled for client 1 with circuit page-1-abc123
RequestServer: DNS lookup successful
RequestServer: Using proxy socks5h://localhost:9050 for request to https://check.torproject.org
```

This confirms:
1. ✅ Tor was enabled
2. ✅ Circuit ID was generated
3. ✅ Proxy configuration was applied to the request
