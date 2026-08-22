(() => {
    const install = () => {
        const control = document.getElementById('control');
        const bridge = window.chrome && window.chrome.webview;
        if (!control || !bridge) return;

        const historyVersionKey = 'cpe-accel-history-version';
        if (localStorage.getItem(historyVersionKey) !== '2') {
            localStorage.removeItem('cpe-accel-history');
            localStorage.setItem(historyVersionKey, '2');
        }
        let hasCpeReply = false;
        let availableDevices = [];
        let previousActiveDevices = [];
        const selectedStorageKey = 'cpe-accel-devices';
        const selectedDeviceIds = () => new Set((localStorage.getItem(selectedStorageKey) || '').split(',').filter(Boolean));
        const renderAvailableDevices = () => {
            const picker = control.querySelector('.accel-device-picker');
            if (!picker || picker.hidden) return;
            const selected = selectedDeviceIds();
            picker.replaceChildren();
            availableDevices.forEach(device => {
                const item = document.createElement('button');
                const info = document.createElement('div');
                const name = document.createElement('b');
                const detail = document.createElement('span');
                const check = document.createElement('i');
                const id = String(device.id || device.mac || device.name);
                item.type = 'button';
                item.className = 'accel-device-option' + (selected.has(id) ? ' selected' : '');
                item.setAttribute('aria-pressed', String(selected.has(id)));
                name.textContent = device.name || 'Unknown device';
                detail.textContent = device.detail || device.mac || '';
                check.className = 'accel-device-check';
                info.append(name, detail);
                item.append(info, check);
                item.addEventListener('click', event => {
                    event.stopPropagation();
                    selected.has(id) ? selected.delete(id) : selected.add(id);
                    localStorage.setItem(selectedStorageKey, [...selected].join(','));
                    try {
                        selectedAccelDevices.clear();
                        selected.forEach(value => selectedAccelDevices.add(value));
                    } catch (_) {}
                    renderAvailableDevices();
                });
                picker.append(item);
            });
        };
        const syncHistory = (activeDevices, cpeHistory) => {
            try {
                const scene = localStorage.getItem('cpe-accel-filter-0') || 'Game';
                if (Array.isArray(cpeHistory)) {
                    const history = cpeHistory.map(device => {
                        const historicalDevice = { ...device, duration: device.totalDuration || device.duration };
                        return {
                            id: String(device.id || device.mac || device.name),
                            name: device.name || 'Unknown device',
                            scene,
                            time: typeof formatAccelDuration === 'function' ? formatAccelDuration(historicalDevice) : (historicalDevice.duration || '0')
                        };
                    });
                    previousActiveDevices = activeDevices;
                    localStorage.setItem('cpe-accel-history', JSON.stringify(history));
                    accelHistory = history;
                    renderAccelRecords();
                    return;
                }
                const activeIds = new Set(activeDevices.map(device => String(device.id || device.mac || device.name)));
                let history = JSON.parse(localStorage.getItem('cpe-accel-history') || '[]');
                history = history.filter(record => !activeIds.has(String(record.id || record.name)));
                const ended = previousActiveDevices.filter(device => !activeIds.has(String(device.id || device.mac || device.name)));
                history = [
                    ...ended.map(device => {
                        const historicalDevice = { ...device, duration: device.totalDuration || device.duration };
                        return {
                            id: String(device.id || device.mac || device.name),
                            name: device.name || 'Unknown device',
                            scene,
                            time: typeof formatAccelDuration === 'function' ? formatAccelDuration(historicalDevice) : (historicalDevice.duration || '0')
                        };
                    }),
                    ...history
                ].slice(0, 30);
                previousActiveDevices = activeDevices;
                localStorage.setItem('cpe-accel-history', JSON.stringify(history));
                accelHistory = history;
                renderAccelRecords();
            } catch (_) {}
        };
        const clearPlaceholder = () => {
            if (hasCpeReply) return;
            const records = control.querySelector('.accel-records');
            const empty = control.querySelector('.accel-empty');
            if (records) records.replaceChildren();
            if (empty) empty.hidden = false;
        };
        bridge.addEventListener('message', event => {
            const data = event.data;
            if (!data || data.action !== 'cpe-acceleration-devices') return;
            hasCpeReply = true;
            if (data.status === 'ok' && Array.isArray(data.availableDevices)) {
                availableDevices = data.availableDevices;
                renderAvailableDevices();
            }
            if (data.status === 'ok' && Array.isArray(data.devices)) syncHistory(data.devices, data.historyStatus === 'ok' && Array.isArray(data.history) ? data.history : null);
        });
        clearPlaceholder();
        setInterval(clearPlaceholder, 100);
        setInterval(() => {
            if (control.classList.contains('active')) bridge.postMessage(JSON.stringify({ action: 'refresh-acceleration-devices' }));
        }, 3000);
        control.addEventListener('click', event => {
            const button = event.target.closest('.accel-dropdown-menu button');
            const menus = control.querySelectorAll('.accel-dropdown-menu');
            if (button && button.parentElement === menus[0]) {
                event.preventDefault();
                event.stopImmediatePropagation();
                const status = control.querySelector('.control-status');
                if (status) {
                    status.textContent = 'This CPE firmware does not expose an acceleration-scene write API.';
                    status.classList.add('error');
                }
            } else if (button && button.parentElement === menus[1]) {
                setTimeout(renderAvailableDevices, 0);
            }
        }, true);
    };
    document.addEventListener('DOMContentLoaded', install, { once: true });
})();
