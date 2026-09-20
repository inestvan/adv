"""Export netlist with kicad-cli and assert the expected connectivity."""
import subprocess, sys, os, xml.etree.ElementTree as ET
HW = os.path.join(os.path.dirname(__file__), '..', 'hw')
out = '/tmp/claude-0/-home-user-adv/6d14bf18-2ba7-5440-bb79-913415086858/scratchpad/net.xml'
subprocess.run(['kicad-cli', 'sch', 'export', 'netlist', '--format', 'kicadxml', '-o', out, os.path.join(HW, 'adv7280m-csi2.kicad_sch')], check=True, capture_output=True)
nets = {}
for n in ET.parse(out).getroot().find('nets'):
    nets[n.get('name')] = set(f"{nd.get('ref')}.{nd.get('pin')}" for nd in n.findall('node'))
def net_of(node):
    for k, v in nets.items():
        if node in v: return k
    return None
def same(*nodes):
    ks = {net_of(n) for n in nodes}
    assert len(ks) == 1 and None not in ks, f"not same net: {nodes} -> {ks}"
def diff(a, b):
    assert net_of(a) != net_of(b), f"unexpected short: {a} {b} on {net_of(a)}"
def exact(name, *nodes):
    assert nets.get(name) == set(nodes), f"{name}: {sorted(nets.get(name, []))} != {sorted(nodes)}"
# MIPI / I2C / control
exact('/CSI_D0P', 'U1.9', 'J1.3'); exact('/CSI_D0N', 'U1.10', 'J1.2'); exact('/CSI_CLKP', 'U1.11', 'J1.9'); exact('/CSI_CLKN', 'U1.12', 'J1.8')
exact('/SDA', 'U1.30', 'J1.14', 'R6.2', 'TP3.1'); exact('/SCL', 'U1.31', 'J1.13', 'R7.2', 'TP4.1')
exact('/~{RESET}', 'U1.28', 'R3.2', 'C5.1', 'JP2.1', 'TP5.1'); exact('/PWRDWN', 'U1.32', 'R4.2', 'JP3.1')
exact('/ALSB', 'U1.29', 'R5.2', 'JP1.2'); exact('/CAM_GPIO', 'J1.11', 'JP2.2', 'JP3.2')
# analog
same('R1.2', 'R2.1', 'C1.1', 'TP7.1'); same('C1.2', 'U1.17')
same('U1.19', 'C4.1'); same('U1.20', 'C4.2'); diff('U1.19', 'U1.20'); diff('U1.20', 'U1.14'); diff('U1.19', 'U1.14')
same('U1.14', 'Y1.2', 'C2.2'); same('U1.15', 'Y1.1', 'C3.2'); diff('U1.14', 'U1.15')
# power
same('+3V3', ) if False else None
assert {'J1.15', 'U1.2', 'U2.1', 'U2.3', 'U3.1', 'U3.3', 'C6.1', 'C13.1', 'C14.1', 'R3.1', 'R4.1', 'R5.1', 'R6.1', 'R7.1', 'TP1.1'} <= nets['+3V3'], nets['+3V3']
assert {'U2.5', 'U1.3', 'U1.13', 'C7.1', 'C8.1', 'C15.1', 'C16.1', 'C17.1', 'C18.1', 'TP2.1'} == nets['+1V8D'], nets['+1V8D']
assert {'U3.5', 'U1.21', 'C9.1', 'C10.1', 'C19.1', 'C20.1', 'FB1.1', 'TP8.1'} == nets['+1V8A'], nets['+1V8A']
assert {'FB1.2', 'C11.1', 'U1.16', 'C21.1', 'C22.1'} == nets['PVDD'], nets['PVDD']
for n in ['U1.1', 'U1.4', 'U1.33', 'U2.2', 'U3.2', 'J1.1', 'J1.4', 'J1.7', 'J1.10', 'J2.2', 'R2.2', 'D1.2', 'C2.1', 'C3.1', 'C5.2', 'JP1.1', 'TP6.1',
          'C6.2', 'C7.2', 'C8.2', 'C9.2', 'C10.2', 'C11.2'] + [f'C{i}.2' for i in range(13, 23)]:
    assert n in nets['GND'], n
# stage 5: buffer section
exact('/5V_IN', 'J4.1', 'D2.2'); exact('/5V_BUF', 'D2.1', 'C28.1', 'FB2.1'); exact('/VS_BUF', 'FB2.2', 'C27.1', 'C26.1', 'TP9.1', 'U5.4')
exact('/CVBS_SRC', 'J5.1', 'R8.1', 'D3.1', 'C23.1', 'C24.1', 'C25.1')
same('C23.2', 'U5.1'); same('C24.2', 'U5.2'); same('C25.2', 'U5.3'); diff('U5.1', 'U5.2'); diff('U5.2', 'U5.3')
same('U5.8', 'R9.1'); same('U5.7', 'R10.1'); same('U5.6', 'R11.1')
exact('/CVBS_BUF', 'R9.2', 'J8.1', 'JP4.1'); exact('/CVBS_IN', 'J2.1', 'D1.1', 'R1.1', 'JP4.2')
exact('/MON1', 'R10.2', 'J6.1'); exact('/MON2', 'R11.2', 'J7.1')
for n in ['U5.5', 'J4.2', 'J5.2', 'J6.2', 'J7.2', 'J8.2', 'R8.2', 'D3.2', 'C26.2', 'C27.2', 'C28.2']:
    assert n in nets['GND'], n
# nothing else unexpectedly connected
for k, v in nets.items():
    if k.startswith('unconnected'): assert len(v) == 1, (k, v)
print('netlist OK:', len(nets), 'nets')
