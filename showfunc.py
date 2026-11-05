import sys, glob
name, n = sys.argv[1], int(sys.argv[2])
for f in glob.glob('ppc/*.cpp'):
    L = open(f, errors='replace').read().split('\n')
    for i, l in enumerate(L):
        if l.startswith('PPC_FUNC_IMPL(__imp__' + name + ')'):
            print(f, i + 1); print('\n'.join(L[i:i + n]))