ffrdp ��һ������ udp �Ŀ��ٿɿ�Э��

rto ���㣺
��ʼ��
rtts = rttm;
rttd = rttm / 2;

������
rtts = (1 - alpha) * rtts + alpha * rttm; // alpha = 1 / 8
rttd = (1 - beta ) * rttd + beta  * abs(rttm - rtts); // beta = 1 / 4

������
rto  = rtts + r * rttd;

��ʱ��
rto  = 1.5 * rto;


֡���壺
data frame: 0x00 seq0 seq1 seq2 data ...
ack  frame: 0x01 una0 una1 una2 mack0 mack1 wind0 wind1
win0 frame: 0x02
win1 frame: 0x03 wind0 wind1
bye  frame: 0x04


Э���ص㣺
ѡ���ش��������ش������ӳ� ACK��UNA + MACK������������


Э��˵����
seq una ����Ϊ 24bit��recv_win_size Ϊ 16bit
ack ֡������ una, mack �� recv_win_size ��Ϣ
mack 16bit ��һ�� bitmap, ������ una ֮�󣬵����Ѿ��� ack ��֡��
win0 �� win1 �������ڲ�ѯ��Ӧ��Է��� recv_win_size
bye �������ڿͻ��˸���������˵�ټ�

���磺una: 16, mack: 0x0003 ���Ӧ�����
ack 1  1  1  1  0  1  1  0  0  0  0
seq 12 13 14 15 16 17 18 19 20 21 22 ...
��Щ֡�Ѿ������շ��յ���Ӧ��

una+mack �ķ�ʽ������ѡ���ش��Ϳ����ش�


rockcarry
2020-9-1



