import matplotlib.pyplot as plt
import numpy as np

times = [
    40.45,
    24.28,
    16.76,
    12.73,
    10.05,
    8.47,
    7.35,
    6.55,
    5.89,
    5.48,
    5.00,
    4.63,
    4.18,
    3.92,
    3.71,
    3.52,
    3.50,
    3.40,
    3.36,
    3.27,
    3.18,
    3.14,
    3.10,
    3.01,
    2.98,
    2.90,
    2.90,
    2.86,
    2.83,
    2.76,
    2.71,
    2.66,
    2.58,
    2.59,
    2.56,
    2.55,
    2.44,
    2.43,
    2.39,
    2.37,
    2.37,
    2.38,
    2.32,
    2.32,
    2.32,
    2.32,
    2.33,
    2.31,
    2.28,
    2.23,
    2.25,
    2.22,
    2.23,
    2.23,
    2.20,
    2.18,
    2.14,
    2.14,
    2.15,
    2.16,
    2.17,
    2.11,
    2.06,
    2.07
]

plt.figure(figsize=(5,4))
plt.xscale("log")
plt.xticks([1, 2, 4, 8, 16, 32, 64], ["1", "2", "4", "8", "16", "32", "64"])
plt.xlabel("#threads")
plt.ylabel("time (secs)")
dim = np.arange(1, 65)

plt.plot(dim, times)

plt.show()
# plt.close()
