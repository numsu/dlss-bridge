# -*- mode: python ; coding: utf-8 -*-
a=Analysis(['../../controller/dlss_bridge.py'],pathex=['../../controller'],hiddenimports=['runtime_setup', 'session'])
pyz=PYZ(a.pure)
exe=EXE(pyz,a.scripts,a.binaries,a.datas,[],name='dlss-bridge',debug=False,bootloader_ignore_signals=False,strip=False,upx=False,console=True)
