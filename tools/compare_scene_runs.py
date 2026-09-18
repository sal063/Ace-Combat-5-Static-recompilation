import re
import sys

PAT = re.compile(r'^scene: field (\d+)\s+state (\S+)')


def load(path):
    out = []
    with open(path, errors='replace') as fp:
        for line in fp:
            m = PAT.match(line)
            if m:
                out.append((int(m.group(1)), m.group(2)))
    return out


def main(a_path, b_path):
    a, b = load(a_path), load(b_path)
    same = [s for _, s in a] == [s for _, s in b]
    shift = max((abs(x[0] - y[0]) for x, y in zip(a, b)), default=0)
    print('transitions: %d vs %d; sequences %s; largest field shift %d'
          % (len(a), len(b), 'IDENTICAL' if same else 'DIFFER', shift))
    if not same:
        for i, (x, y) in enumerate(zip(a, b)):
            if x[1] != y[1]:
                print('first difference at transition %d: %s (field %d) vs %s '
                      '(field %d)' % (i, x[1], x[0], y[1], y[0]))
                break
    return 0 if same else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
