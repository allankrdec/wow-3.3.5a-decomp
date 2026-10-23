#!/usr/bin/env python3
# Liga a flag 0x400 (área com voo) em todas as áreas de Kalimdor (mapa 1)
# e dos Reinos do Leste (mapa 0) de um AreaTable.dbc 3.3.5a.
#
# Uso: python3 liberar-voo-areatable.py AreaTable.dbc AreaTable-voo.dbc
#
# O voo precisa ser liberado nos dois lados:
#   - Servidor (TrinityCore): trocar <DataDir>/dbc/AreaTable.dbc pela saída
#     deste script e reiniciar o worldserver.
#   - Cliente: uma MPQ de patch com DBFilesClient\AreaTable.dbc gerado a partir
#     da versão que o cliente usa (a da patch-6.MPQ, traduzida). Já existe uma
#     pronta em Data/patch-8-voar.MPQ.disable; para ativar, renomear para
#     patch-8.MPQ (o cliente só carrega nomes no padrão patch-?.MPQ).
#
# O mundo antigo não foi feito para voo: falta colisão e há construções
# incompletas, igual no cliente original.
import struct
import sys

AREA_FLAG_FLYABLE = 0x400
FLYABLE_MAPS = (0, 1)

src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, 'rb').read())
magic, count, fields, rowSize, _ = struct.unpack_from('<4s4I', data)
if magic != b'WDBC' or fields != 36:
    sys.exit('não parece um AreaTable.dbc do 3.3.5a')

changed = 0
for i in range(count):
    row = 20 + i * rowSize
    mapId, = struct.unpack_from('<I', data, row + 4)
    flags, = struct.unpack_from('<I', data, row + 16)
    if mapId in FLYABLE_MAPS and not flags & AREA_FLAG_FLYABLE:
        struct.pack_into('<I', data, row + 16, flags | AREA_FLAG_FLYABLE)
        changed += 1

open(dst, 'wb').write(data)
print('%d áreas alteradas' % changed)
