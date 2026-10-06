import json,glob,os,sys
S={os.path.basename(f)[:-5]:json.load(open(f)) for f in glob.glob('sheets/*.json')}
key={'game_controls':'name','inputs':'action','arms':'side','natives':'name','hooks':'name','systems':'name','settings':'name'}
names={s:{r[key[s]] for r in S[s]['rows']} for s in S}
empty=[];unver=[];bad=[]
for s,d in S.items():
  for r in d['rows']:
    rid=r[key[s]]
    if s=='systems' and not r.get('in_v01'): continue
    for c in d['columns']:
      if c not in r or r[c] is None or r[c]=='' : empty.append(f"{s}.{rid}.{c}")
    if r.get('verified') is not True: unver.append(f"{s}.{rid}")
def ref(src,val,target):
  if val in ('-',None): return
  if val not in names[target]: bad.append(f"{src} -> {target}:{val}")
for r in S['inputs']['rows']:
  ref(f"inputs.{r['action']}.game_control",r['game_control'],'game_controls')
  ref(f"inputs.{r['action']}.game_control_vehicle",r['game_control_vehicle'],'game_controls')
for r in S['arms']['rows']: ref(f"arms.{r['side']}.pose_action",r['pose_action'],'inputs')
for r in S['systems']['rows']:
  for t in ('natives','hooks','inputs','arms'):
    for v in r[t]: ref(f"systems.{r['name']}.{t}",v,t)
print("UNFILLED CELLS (%d):"%len(empty)); [print("  ",e) for e in empty]
print("BROKEN REFERENCES (%d):"%len(bad)); [print("  ",e) for e in bad]
print("ROWS NOT VERIFIED IN GAME: %d"%len(unver))
sys.exit(1 if empty or bad else 0)
