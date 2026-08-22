(() => {
    const icon = name => {
        const paths = {
            home: '<path d="M3 10.5 12 3l9 7.5v8a1.5 1.5 0 0 1-1.5 1.5H15v-6H9v6H4.5A1.5 1.5 0 0 1 3 18.5z"/>',
            control: '<path d="M4 6h16M4 12h16M4 18h16"/><circle cx="9" cy="6" r="2"/><circle cx="15" cy="12" r="2"/><circle cx="7" cy="18" r="2"/>',
            lock: '<rect x="5" y="10" width="14" height="10" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3M12 14v3"/>',
            params: '<rect x="5" y="3" width="14" height="18" rx="2"/><path d="M9 3.5h6v3H9zM9 11h6M9 15h6"/>',
            sms: '<path d="M4 5.5A2.5 2.5 0 0 1 6.5 3h11A2.5 2.5 0 0 1 20 5.5v8a2.5 2.5 0 0 1-2.5 2.5H10l-4.5 4v-4.2A2.5 2.5 0 0 1 4 13.5z"/><path d="M8 8h8M8 11.5h5"/>'
        };
        return '<svg viewBox="0 0 24 24" aria-hidden="true">' + paths[name] + '</svg>';
    };
    const eye = visible => '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M2.5 12s3.2-5 9.5-5 9.5 5 9.5 5-3.2 5-9.5 5-9.5-5-9.5-5Z"/><circle cx="12" cy="12" r="2.5"/>' + (visible ? '' : '<path d="m4 4 16 16"/>') + '</svg>';
    const empty = '\u2014';
    const mask = value => {
        const text = String(value || '');
        return !text ? empty : text.length <= 4 ? '\u2022\u2022\u2022\u2022' : text.slice(0, 2) + '\u2022\u2022\u2022\u2022\u2022' + text.slice(-2);
    };
    const duration = value => {
        const seconds = Number(value);
        if (!Number.isFinite(seconds) || seconds < 0 || !String(value).trim()) return value || empty;
        const day = Math.floor(seconds / 86400), hour = Math.floor(seconds % 86400 / 3600), minute = Math.floor(seconds % 3600 / 60);
        return (day ? day + '\u5929' : '') + (hour ? hour + '\u65f6' : '') + minute + '\u5206';
    };
    const install = () => {
        const panel = document.getElementById('params');
        const bridge = window.chrome && window.chrome.webview;
        if (!panel || !bridge) return;
        const style = document.createElement('style');
        style.textContent = '.nav{top:13px!important;bottom:auto!important}#params{padding:80px 14px 20px!important;background:linear-gradient(145deg,#f6fbff,#fdfbf0)}#params .param-card{margin:0 0 10px;padding:13px 12px 8px;border:1px solid #e3e8ed;border-radius:21px;background:rgba(255,255,255,.84);box-shadow:0 6px 14px rgba(74,92,105,.07)}#params .param-card h1{margin:0 2px 9px;color:#17202d;font-size:16px}#params .param-list{overflow:hidden;border:1px solid #e7ecf0;border-radius:16px;background:rgba(252,253,253,.72)}#params .param-row{display:grid;grid-template-columns:minmax(94px,.9fr) minmax(0,1.28fr) 27px;gap:6px;align-items:center;min-height:42px;padding:7px 9px;border-top:1px solid #e9edef}#params .param-row:first-child{border-top:0}#params .param-row b{color:#17202d;font-size:12px;line-height:1.26}#params .param-value{min-width:0;color:#788596;font-size:12px;line-height:1.32;overflow-wrap:anywhere}#params .param-eye{display:grid;place-items:center;width:27px;height:27px;border:0;border-radius:14px;background:transparent;color:#8996a7;cursor:pointer}#params .param-eye svg,.nav .tab i svg{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:1.9;stroke-linecap:round;stroke-linejoin:round}#params .param-eye:active{background:#e9f4ff;color:#2d85e8}#params .param-row button[hidden]{display:none}#params .param-status{min-height:15px;margin:6px 4px;color:#7d8998;font-size:10px;text-align:center}#params .param-status.error{color:#d96565}.nav .tab i{display:grid;place-items:center;height:22px;margin:0 auto 2px;font-style:normal}.nav .tab i svg{width:20px;height:20px}';
        document.head.append(style);
        const navIcons = ['home', 'control', 'lock', 'params', 'sms', null];
        document.querySelectorAll('.nav .tab').forEach((tab, index) => {
            const target = tab.querySelector('i');
            if (target && navIcons[index]) target.innerHTML = icon(navIcons[index]);
        });
        const deviceRows = [
            ['\u8bbe\u5907\u578b\u53f7', 'model', false], ['\u5f00\u673a\u65f6\u957f', 'uptime', false],
            ['\u5e8f\u5217\u53f7', 'serialNumber', true], ['IMEI', 'imei', true],
            ['IMSI', 'imsi', true], ['\u672c\u673a\u53f7\u7801', 'phoneNumber', true]
        ];
        const versionRows = [
            ['\u786c\u4ef6\u7248\u672c\u53f7', 'hardwareVersion'], ['\u8f6f\u4ef6\u7248\u672c\u53f7', 'softwareVersion'],
            ['Web UI \u7248\u672c\u53f7', 'webUiVersion'], ['\u914d\u7f6e\u6587\u4ef6\u7248\u672c\u53f7', 'configVersion'],
            ['\u53c2\u6570\u7248\u672c\u53f7', 'parameterVersion']
        ];
        const basebandRows = [
            ['CELL_ID', 'cellId'], ['5G MCS \u4e0a\u884c\u8c03\u5236\u89e3\u8c03\u65b9\u5f0f', 'mcsUp'],
            ['5G MCS \u4e0b\u884c\u8c03\u5236\u89e3\u8c03\u65b9\u5f0f', 'mcsDown'], ['5G RANK', 'rank'],
            ['5G CQI', 'cqi'], ['PLMN', 'plmn'], ['Band', 'bandSummary']
        ];
        const values = {};
        const card = (title, rows) => {
            const section = document.createElement('section');
            section.className = 'param-card';
            const heading = document.createElement('h1');
            heading.textContent = title;
            const list = document.createElement('div');
            list.className = 'param-list';
            rows.forEach(([label, key, protectedValue]) => {
                const row = document.createElement('div');
                row.className = 'param-row';
                const name = document.createElement('b');
                name.textContent = label;
                const value = document.createElement('span');
                value.className = 'param-value';
                value.dataset.key = key;
                const toggle = document.createElement('button');
                toggle.type = 'button';
                if (protectedValue) {
                    toggle.className = 'param-eye';
                    toggle.dataset.shown = 'false';
                    toggle.setAttribute('aria-label', '\u663e\u793a' + label);
                    toggle.innerHTML = eye(false);
                    toggle.onclick = () => {
                        const shown = toggle.dataset.shown !== 'true';
                        toggle.dataset.shown = String(shown);
                        toggle.setAttribute('aria-label', (shown ? '\u9690\u85cf' : '\u663e\u793a') + label);
                        toggle.innerHTML = eye(shown);
                        value.textContent = shown ? (values[key] || empty) : mask(values[key]);
                    };
                } else toggle.hidden = true;
                row.append(name, value, toggle);
                list.append(row);
            });
            section.append(heading, list);
            return section;
        };
        panel.replaceChildren(card('\u8bbe\u5907\u4fe1\u606f', deviceRows), card('\u7248\u672c\u4fe1\u606f', versionRows), card('\u57fa\u5e26\u4fe1\u606f', basebandRows), card('\u8fdb\u7f51\u6807\u5fd7', [['\u8fdb\u7f51\u6807\u5fd7\u4ee3\u7801', 'accessCode']]));
        const render = data => {
            deviceRows.concat(versionRows, basebandRows, [['', 'accessCode']]).forEach(([, key, sensitive]) => {
                if (!Object.prototype.hasOwnProperty.call(data, key)) return;
                values[key] = data[key] || '';
                const node = panel.querySelector('[data-key="' + key + '"]');
                if (!node) return;
                const toggle = node.parentElement.querySelector('.param-eye');
                const shown = toggle && toggle.dataset.shown === 'true';
                node.textContent = key === 'uptime' ? duration(values[key]) : sensitive && !shown ? mask(values[key]) : values[key] || empty;
            });
        };
        const request = () => {
            bridge.postMessage(JSON.stringify({ action: 'refresh-device-information' }));
        };
        bridge.addEventListener('message', event => {
            const data = event.data;
            if (!data || data.action !== 'cpe-device-information') return;
            if (data.status === 'ok') {
                render(data);
            }
        });
        document.querySelector('[data-page="params"]').addEventListener('click', request);
        bridge.addEventListener('message', event => {
            const data = event.data;
            if (!data || data.action !== 'cpe-data' || data.status !== 'ok') return;
            render(data);
        });
        document.querySelector('[data-page="params"]').addEventListener('click', () => bridge.postMessage(JSON.stringify({ action: 'refresh-cpe' })));
        request();
    };
    document.addEventListener('DOMContentLoaded', install, { once: true });
})();
