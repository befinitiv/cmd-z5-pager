import sys,copy
from sexp import *
LIBDIR="/usr/share/kicad/symbols/"
_cache={}
def lib(name):
    if name not in _cache: _cache[name]=parse(open(LIBDIR+name+".kicad_sym").read())[0]
    return _cache[name]
def get_symbol(libname,symname):
    L=lib(libname)
    syms={e[1]:e for e in find(L,'symbol')}
    s=copy.deepcopy(syms[symname])
    ext=find1(s,'extends')
    if ext:
        parent=get_symbol(libname,ext[1])
        # flatten: parent's body with child's properties
        newp=[Sym('symbol'),symname]
        childprops={p[1]:p for p in find(s,'property')}
        for e in parent[2:]:
            if isinstance(e,list) and e[0]=='property':
                newp.append(childprops.pop(e[1],e))
            elif isinstance(e,list) and e[0]=='symbol':
                e=copy.deepcopy(e); e[1]=e[1].replace(ext[1],symname,1); newp.append(e)
            else: newp.append(e)
        for p in childprops.values(): newp.insert(2,p)
        s=newp
    return s
def pins(sym):
    out=[]
    def walk(x):
        for e in x:
            if isinstance(e,list):
                if e and e[0]=='pin':
                    at=find1(e,'at'); nm=find1(e,'name'); nu=find1(e,'number')
                    hide = any(isinstance(z,list) and z[0]=='hide' for z in e) or Sym('hide') in e
                    out.append(dict(type=str(e[1]),x=float(at[1]),y=float(at[2]),ang=int(float(at[3])),name=nm[1],num=nu[1],hide=hide,length=float(find1(e,'length')[1])))
                else: walk(e)
    walk(sym); return out
if __name__=="__main__":
    for a in sys.argv[1:]:
        l,s=a.split(':')
        sy=get_symbol(l,s)
        print("==",a,[ (p[1],p[2]) for p in find(sy,'property')][:3])
        for p in pins(sy): print("  ",p['num'],p['name'],p['type'],p['x'],p['y'],p['ang'],'H' if p['hide'] else '')
