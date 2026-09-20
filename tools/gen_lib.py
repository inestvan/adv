"""Generate hw/lib/adv-parts.kicad_sym: ADV7280WBCPZ-M symbol and project power symbols."""
import os, sys, copy
sys.path.insert(0, os.path.dirname(__file__))
from kisexp import *

KSYM = '/usr/share/kicad/symbols'

def eff(size=1.27, hide=False, justify=None):
    e = S('effects', S('font', S('size', size, size)))
    if justify: e.append(S('justify', Sym(justify)))
    if hide: e.append(S('hide', Sym('yes')))
    return e

def prop(name, val, x=0, y=0, hide=False, justify=None):
    return S('property', name, val, S('at', x, y, 0), eff(hide=hide, justify=justify))

def pin(etype, num, name, x, y, ang, length=2.54):
    return S('pin', Sym(etype), Sym('line'), S('at', x, y, ang), S('length', length),
             S('name', name, eff()), S('number', num, eff()))

def adv7280m():
    name = 'ADV7280WBCPZ-M'
    L, R, T, B = -20.32, 20.32, 25.4, -25.4
    pins = []
    # left: analog
    left = [('17','AIN1'),('18','AIN2'),('22','AIN3'),('23','AIN4'),('24','AIN5'),('25','AIN6'),('26','AIN7'),('27','AIN8')]
    for i,(n,nm) in enumerate(left):
        pins.append(pin('passive', n, nm, L, 20.32 - 2.54*i, 0))
    pins.append(pin('passive', '19', 'VREFP', L, -2.54, 0))
    pins.append(pin('passive', '20', 'VREFN', L, -5.08, 0))
    pins.append(pin('passive', '14', 'XTALP', L, -10.16, 0))
    pins.append(pin('passive', '15', 'XTALN', L, -12.7, 0))
    # right: digital
    pins.append(pin('output', '9', 'D0P', R, 20.32, 180))
    pins.append(pin('output', '10', 'D0N', R, 17.78, 180))
    pins.append(pin('output', '11', 'CLKP', R, 15.24, 180))
    pins.append(pin('output', '12', 'CLKN', R, 12.7, 180))
    pins.append(pin('bidirectional', '30', 'SDATA', R, 7.62, 180))
    pins.append(pin('input', '31', 'SCLK', R, 5.08, 180))
    pins.append(pin('input', '29', 'ALSB', R, 2.54, 180))
    pins.append(pin('input', '28', '~{RESET}', R, -2.54, 180))
    pins.append(pin('input', '32', '~{PWRDWN}', R, -5.08, 180))
    pins.append(pin('output', '5', 'INTRQ', R, -10.16, 180))
    pins.append(pin('output', '8', 'GPO0', R, -12.7, 180))
    pins.append(pin('output', '7', 'GPO1', R, -15.24, 180))
    pins.append(pin('output', '6', 'GPO2', R, -17.78, 180))
    # top: supplies
    for n,nm,x in [('2','DVDDIO',-10.16),('3','DVDD',-5.08),('21','AVDD',0),('16','PVDD',5.08),('13','MVDD',10.16)]:
        pins.append(pin('power_in', n, nm, x, T, 270))
    # bottom: grounds
    pins.append(pin('power_in', '1', 'DGND', -5.08, B, 90))
    pins.append(pin('power_in', '4', 'DGND', 0, B, 90))
    pins.append(pin('passive', '33', 'EP', 5.08, B, 90))
    sym = S('symbol', name, S('exclude_from_sim', Sym('no')), S('in_bom', Sym('yes')), S('on_board', Sym('yes')),
            prop('Reference', 'U', -17.78, 24.13, justify='left'),
            prop('Value', name, 0, -27.94),
            prop('Footprint', 'Package_DFN_QFN:QFN-32-1EP_5x5mm_P0.5mm_EP3.6x3.6mm', 0, -30.48, hide=True),
            prop('Datasheet', 'https://www.analog.com/media/en/technical-documentation/data-sheets/ADV7280.pdf', 0, -33.02, hide=True),
            prop('Description', 'SDTV video decoder with MIPI CSI-2 output, LFCSP-32 5x5 mm (CP-32-12)', 0, -35.56, hide=True),
            S('property', 'ki_keywords', 'video decoder CVBS MIPI CSI-2', S('at', 0, 0, 0), eff(hide=True)),
            S('property', 'ki_fp_filters', 'QFN*1EP*5x5mm*P0.5mm*', S('at', 0, 0, 0), eff(hide=True)),
            S('symbol', name + '_0_1',
              S('rectangle', S('start', -17.78, 22.86), S('end', 17.78, -22.86),
                S('stroke', S('width', 0.254), S('type', Sym('default'))), S('fill', S('type', Sym('background'))))),
            S('symbol', name + '_1_1', *pins))
    return sym

def power_symbol(name, base='+1V8', desc=None):
    lib = load_lib(os.path.join(KSYM, 'power.kicad_sym'))
    s = copy.deepcopy(get_symbol(lib, base))
    s[1] = name
    for c in s:
        if isinstance(c, list) and c[0] == 'symbol':
            c[1] = name + c[1][len(base):]
        if isinstance(c, list) and c[0] == 'property':
            if c[1] == 'Value': c[2] = name
            if c[1] == 'Description' and desc: c[2] = desc
    return s

def build():
    root = S('kicad_symbol_lib', S('version', 20241209), S('generator', 'kicad_symbol_editor'), S('generator_version', '9.0'))
    root.append(adv7280m())
    root.append(power_symbol('+1V8D', desc='1.8 V digital rail (DVDD, MVDD) from U2'))
    root.append(power_symbol('+1V8A', desc='1.8 V analog rail (AVDD) from U3'))
    root.append(power_symbol('PVDD', desc='1.8 V PLL rail, filtered from +1V8A'))
    return root

if __name__ == '__main__':
    out = os.path.join(os.path.dirname(__file__), '..', 'hw', 'lib', 'adv-parts.kicad_sym')
    with open(out, 'w', encoding='utf-8') as f:
        f.write(dump(build()) + '\n')
    print('wrote', out)
