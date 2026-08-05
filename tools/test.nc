( grblhal-clearcore SD job test — small safe moves, all 4 axes )
G21 G90 G94
G0 X0 Y0 Z0 A0
G1 X5 F300
G1 Y5
G1 Z-2 F150
G1 A90 F600
G4 P0.5
G1 Z0 F150
G1 A0 F600
G0 X0 Y0
M2
