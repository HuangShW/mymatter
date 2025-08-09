## Dimmable Light

> A Dimmable Light is a lighting device that is capable of being switched on or off and the intensity of its light adjusted by means of a bound controller device such as a Dimmer Switch or a Color Dim­ mer Switch. In addition, a Dimmable Light device is also capable of being switched by means of a bound occupancy sensor or other device(s).

### Revision History

| **Revision** | **Description** |
|----|----|
| 1 | Initial Zigbee 3.0 revision |
| 2 | New data model format and notation |
| 3 | Updated the Scenes cluster to Scenes Manage­ ment with Cluster ID: 0x0062 |

### Classification

| **ID** | **Device Name** | **Superset** | **Class** | **Scope** |
|--------|-----------------|--------------|-----------|-----------|
| 0x0101 | Dimmable Light  | On/Off Light | Simple    | Endpoint  |

### Conditions

> Please see the Base Device Type definition for conformance tags.

### Cluster Requirements

> Each endpoint supporting this device type SHALL include these clusters based on the conformance defined below.
>
> *Table 2. Dimmable Light Cluster Requirements*

| **ID** | **Cluster**        | **Client/Server** | **Quality** | **Conformance** |
|--------|--------------------|-------------------|-------------|-----------------|
| 0x0003 | Identify           | Server            |             | M               |
| 0x0004 | Groups             | Server            |             | M               |
| 0x0062 | Scenes Manage­ ment | Server            |             | P, M            |
| 0x0006 | On/Off             | Server            |             | M               |
| 0x0008 | Level Control      | Server            |             | M               |
| 0x0406 | Occupancy Sens­ ing | Client            |             | O               |

### Element Requirements

> Below list qualities and conformance that override the cluster specification requirements. A blank table cell means there is no change to that item and the value from the cluster specification applies.

| **ID** | **Cluster** | **Element** | **Name** | **Constraint** | **Access** | **Confor­ mance** |
|----|----|----|----|----|----|----|
| 0x0003 | Identify | Command | TriggerEffect |  |  | M |
| 0x0062 | Scenes Man­ agement | Command | CopyScene |  |  | P, M |
| 0x0006 | On/Off | Feature | Lighting |  |  | M |

| **ID** | **Cluster** | **Element** | **Name** | **Constraint** | **Access** | **Confor­ mance** |
|----|----|----|----|----|----|----|
| 0x0008 | Level Con­ trol | Feature | Lighting |  |  | M |
| 0x0008 | Level Con­ trol | Feature | OnOff |  |  | M |
| 0x0008 | Level Con­ trol | Attribute | CurrentLevel | 1 to 254 |  |  |
| 0x0008 | Level Con­ trol | Attribute | MinLevel | 1 |  |  |
| 0x0008 | Level Con­ trol | Attribute | MaxLevel | 254 |  |  |



## Color Temperature Light

> A Color Temperature Light is a lighting device that is capable of being switched on or off, the inten­ sity of its light adjusted, and its color temperature adjusted by means of a bound controller device such as a Color Dimmer Switch.

### Revision History

| **Revision** | **Description** |
|----|----|
| 1 | Initial Zigbee 3.0 revision |
| 2 | New data model format and notation |
| 3 | Added optional occupancy sensing |
| 4 | Updated the Scenes cluster to Scenes Manage­ ment with Cluster ID: 0x0062 |

### Classification

| **ID** | **Device Name**          | **Superset**   | **Class** | **Scope** |
|--------|--------------------------|----------------|-----------|-----------|
| 0x010C | Color Tempera­ ture Light | Dimmable Light | Simple    | Endpoint  |

### Conditions

> Please see the Base Device Type definition for conformance tags.

### Cluster Requirements

> Each endpoint supporting this device type SHALL include these clusters based on the conformance defined below.
>
> *Table 3. Color Temperature Light Cluster Requirements*

| **ID** | **Cluster**        | **Client/Server** | **Quality** | **Conformance** |
|--------|--------------------|-------------------|-------------|-----------------|
| 0x0003 | Identify           | Server            |             | M               |
| 0x0004 | Groups             | Server            |             | M               |
| 0x0062 | Scenes Manage­ ment | Server            |             | P, M            |
| 0x0006 | On/Off             | Server            |             | M               |
| 0x0008 | Level Control      | Server            |             | M               |
| 0x0300 | Color Control      | Server            |             | M               |
| 0x0406 | Occupancy Sens­ ing | Client            |             | O               |

### Element Requirements

> Below list qualities and conformance that override the cluster specification requirements. A blank table cell means there is no change to that item and the value from the cluster specification applies.

| **ID** | **Cluster** | **Element** | **Name** | **Constraint** | **Access** | **Confor­ mance** |
|----|----|----|----|----|----|----|
| 0x0003 | Identify | Command | TriggerEffect |  |  | M |
| 0x0062 | Scenes Man­ agement | Command | CopyScene |  |  | P, M |
| 0x0006 | On/Off | Feature | Lighting |  |  | M |
| 0x0008 | Level Con­ trol | Feature | OnOff |  |  | M |
| 0x0008 | Level Con­ trol | Feature | Lighting |  |  | M |
| 0x0008 | Level Con­ trol | Attribute | CurrentLevel | 1 to 254 |  |  |
| 0x0008 | Level Con­ trol | Attribute | MinLevel | 1 |  |  |
| 0x0008 | Level Con­ trol | Attribute | MaxLevel | 254 |  |  |
| 0x0300 | Color Con­ trol | Feature | ColorTem­ perature |  |  | M |
| 0x0300 | Color Con­ trol | Attribute | Remaining­ Time |  |  | M |

