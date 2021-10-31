import matplotlib.pyplot as plt

colour_values = ['b','g','r','c','m','y','k','pink', 'indigo', 'navy', 'purple', 'silver', 'darkgreen', 'gold', 'orange', 'gray']
file = open("./output_file", "r")
queue_values = {}
tick_values = {}
tick_no = 0

for line in file:
    if line[:3] == "PID":
        tick_no+=1
        continue
    if(not(line[0]>='0' and line[0]<='9')):
        continue
    vals = line.split()
    if(len(vals)==0):
        continue
    a=int(vals[0])
    if a not in queue_values:
        queue_values[a]=[]
        tick_values[a]=[]
    queue_values[a] += [int(vals[1])]
    tick_values[a] += [tick_no]
    if tick_no > 400:
        break

plt.figure(figsize=(30, 30))
legen=[]
for pid in queue_values:
    plt.plot(tick_values[pid], queue_values[pid], colour_values[pid])
    legen += ["P"+str(pid)]
plt.legend(legen)
plt.show()
